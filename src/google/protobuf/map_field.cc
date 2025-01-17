// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "google/protobuf/map_field.h"

#include <utility>
#include <vector>

#include "google/protobuf/port.h"
#include "google/protobuf/map_field_inl.h"

// Must be included last.
#include "google/protobuf/port_def.inc"

namespace google {
namespace protobuf {
namespace internal {
using ::google::protobuf::internal::DownCast;

MapFieldBase::~MapFieldBase() {
  ABSL_DCHECK_EQ(arena(), nullptr);
  delete maybe_payload();
}

const RepeatedPtrFieldBase& MapFieldBase::GetRepeatedField() const {
  ConstAccess();
  SyncRepeatedFieldWithMap();
  return reinterpret_cast<RepeatedPtrFieldBase&>(payload().repeated_field);
}

RepeatedPtrFieldBase* MapFieldBase::MutableRepeatedField() {
  MutableAccess();
  SyncRepeatedFieldWithMap();
  SetRepeatedDirty();
  return reinterpret_cast<RepeatedPtrFieldBase*>(&payload().repeated_field);
}

template <typename T>
static void SwapRelaxed(std::atomic<T>& a, std::atomic<T>& b) {
  auto value_b = b.load(std::memory_order_relaxed);
  auto value_a = a.load(std::memory_order_relaxed);
  b.store(value_a, std::memory_order_relaxed);
  a.store(value_b, std::memory_order_relaxed);
}

MapFieldBase::ReflectionPayload& MapFieldBase::PayloadSlow() const {
  auto p = payload_.load(std::memory_order_acquire);
  if (!IsPayload(p)) {
    auto* arena = ToArena(p);
    auto* payload = Arena::Create<ReflectionPayload>(arena, arena);
    auto new_p = ToTaggedPtr(payload);
    if (payload_.compare_exchange_strong(p, new_p, std::memory_order_acq_rel)) {
      // We were able to store it.
      p = new_p;
    } else {
      // Someone beat us to it. Throw away the one we made. `p` already contains
      // the one we want.
      if (arena == nullptr) delete payload;
    }
  }
  return *ToPayload(p);
}

void MapFieldBase::Swap(MapFieldBase* other) {
  if (arena() == other->arena()) {
    InternalSwap(other);
    return;
  }
  auto* p1 = maybe_payload();
  auto* p2 = other->maybe_payload();
  if (p1 == nullptr && p2 == nullptr) return;

  if (p1 == nullptr) p1 = &payload();
  if (p2 == nullptr) p2 = &other->payload();
  p1->repeated_field.Swap(&p2->repeated_field);
  SwapRelaxed(p1->state, p2->state);
}

void MapFieldBase::UnsafeShallowSwap(MapFieldBase* other) {
  ABSL_DCHECK_EQ(arena(), other->arena());
  InternalSwap(other);
}

void MapFieldBase::InternalSwap(MapFieldBase* other) {
  SwapRelaxed(payload_, other->payload_);
}

size_t MapFieldBase::SpaceUsedExcludingSelfLong() const {
  ConstAccess();
  size_t size = 0;
  if (auto* p = maybe_payload()) {
    {
      absl::MutexLock lock(&p->mutex);
      size = SpaceUsedExcludingSelfNoLock();
    }
    ConstAccess();
  }
  return size;
}

size_t MapFieldBase::SpaceUsedExcludingSelfNoLock() const {
  if (auto* p = maybe_payload()) {
    return p->repeated_field.SpaceUsedExcludingSelfLong();
  } else {
    return 0;
  }
}

bool MapFieldBase::IsMapValid() const {
  ConstAccess();
  // "Acquire" insures the operation after SyncRepeatedFieldWithMap won't get
  // executed before state_ is checked.
  return state() != STATE_MODIFIED_REPEATED;
}

bool MapFieldBase::IsRepeatedFieldValid() const {
  ConstAccess();
  return state() != STATE_MODIFIED_MAP;
}

void MapFieldBase::SetMapDirty() {
  MutableAccess();
  // These are called by (non-const) mutator functions. So by our API it's the
  // callers responsibility to have these calls properly ordered.
  payload().state.store(STATE_MODIFIED_MAP, std::memory_order_relaxed);
}

void MapFieldBase::SetRepeatedDirty() {
  MutableAccess();
  // These are called by (non-const) mutator functions. So by our API it's the
  // callers responsibility to have these calls properly ordered.
  payload().state.store(STATE_MODIFIED_REPEATED, std::memory_order_relaxed);
}

void MapFieldBase::SyncRepeatedFieldWithMap() const {
  ConstAccess();
  if (state() == STATE_MODIFIED_MAP) {
    auto& p = payload();
    {
      absl::MutexLock lock(&p.mutex);
      // Double check state, because another thread may have seen the same
      // state and done the synchronization before the current thread.
      if (p.state.load(std::memory_order_relaxed) == STATE_MODIFIED_MAP) {
        SyncRepeatedFieldWithMapNoLock();
        p.state.store(CLEAN, std::memory_order_release);
      }
    }
    ConstAccess();
  }
}

void MapFieldBase::SyncMapWithRepeatedField() const {
  ConstAccess();
  // acquire here matches with release below to ensure that we can only see a
  // value of CLEAN after all previous changes have been synced.
  if (state() == STATE_MODIFIED_REPEATED) {
    auto& p = payload();
    {
      absl::MutexLock lock(&p.mutex);
      // Double check state, because another thread may have seen the same state
      // and done the synchronization before the current thread.
      if (p.state.load(std::memory_order_relaxed) == STATE_MODIFIED_REPEATED) {
        SyncMapWithRepeatedFieldNoLock();
        p.state.store(CLEAN, std::memory_order_release);
      }
    }
    ConstAccess();
  }
}

// ------------------DynamicMapField------------------
DynamicMapField::DynamicMapField(const Message* default_entry)
    : default_entry_(default_entry) {}

DynamicMapField::DynamicMapField(const Message* default_entry, Arena* arena)
    : TypeDefinedMapFieldBase<MapKey, MapValueRef>(arena),
      default_entry_(default_entry) {}

DynamicMapField::~DynamicMapField() {
  ABSL_DCHECK_EQ(arena(), nullptr);
  // DynamicMapField owns map values. Need to delete them before clearing the
  // map.
  for (auto& kv : map_) {
    kv.second.DeleteData();
  }
  map_.clear();
}

void DynamicMapField::Clear() {
  Map<MapKey, MapValueRef>* map = &const_cast<DynamicMapField*>(this)->map_;
  if (arena() == nullptr) {
    for (Map<MapKey, MapValueRef>::iterator iter = map->begin();
         iter != map->end(); ++iter) {
      iter->second.DeleteData();
    }
  }

  map->clear();

  if (auto* p = maybe_payload()) {
    p->repeated_field.Clear();
  }
  // Data in map and repeated field are both empty, but we can't set status
  // CLEAN which will invalidate previous reference to map.
  MapFieldBase::SetMapDirty();
}

void DynamicMapField::AllocateMapValue(MapValueRef* map_val) {
  const FieldDescriptor* val_des = default_entry_->GetDescriptor()->map_value();
  map_val->SetType(val_des->cpp_type());
  // Allocate memory for the MapValueRef, and initialize to
  // default value.
  switch (val_des->cpp_type()) {
#define HANDLE_TYPE(CPPTYPE, TYPE)              \
  case FieldDescriptor::CPPTYPE_##CPPTYPE: {    \
    auto* value = Arena::Create<TYPE>(arena()); \
    map_val->SetValue(value);                   \
    break;                                      \
  }
    HANDLE_TYPE(INT32, int32_t);
    HANDLE_TYPE(INT64, int64_t);
    HANDLE_TYPE(UINT32, uint32_t);
    HANDLE_TYPE(UINT64, uint64_t);
    HANDLE_TYPE(DOUBLE, double);
    HANDLE_TYPE(FLOAT, float);
    HANDLE_TYPE(BOOL, bool);
    HANDLE_TYPE(STRING, std::string);
    HANDLE_TYPE(ENUM, int32_t);
#undef HANDLE_TYPE
    case FieldDescriptor::CPPTYPE_MESSAGE: {
      const Message& message =
          default_entry_->GetReflection()->GetMessage(*default_entry_, val_des);
      Message* value = message.New(arena());
      map_val->SetValue(value);
      break;
    }
  }
}

bool DynamicMapField::InsertOrLookupMapValue(const MapKey& map_key,
                                             MapValueRef* val) {
  // Always use mutable map because users may change the map value by
  // MapValueRef.
  Map<MapKey, MapValueRef>* map = MutableMap();
  Map<MapKey, MapValueRef>::iterator iter = map->find(map_key);
  if (iter == map->end()) {
    MapValueRef& map_val = map_[map_key];
    AllocateMapValue(&map_val);
    val->CopyFrom(map_val);
    return true;
  }
  // map_key is already in the map. Make sure (*map)[map_key] is not called.
  // [] may reorder the map and iterators.
  val->CopyFrom(iter->second);
  return false;
}

void DynamicMapField::MergeFrom(const MapFieldBase& other) {
  ABSL_DCHECK(IsMapValid() && other.IsMapValid());
  Map<MapKey, MapValueRef>* map = MutableMap();
  const DynamicMapField& other_field =
      reinterpret_cast<const DynamicMapField&>(other);
  for (Map<MapKey, MapValueRef>::const_iterator other_it =
           other_field.map_.begin();
       other_it != other_field.map_.end(); ++other_it) {
    Map<MapKey, MapValueRef>::iterator iter = map->find(other_it->first);
    MapValueRef* map_val;
    if (iter == map->end()) {
      map_val = &map_[other_it->first];
      AllocateMapValue(map_val);
    } else {
      map_val = &iter->second;
    }

    // Copy map value
    const FieldDescriptor* field_descriptor =
        default_entry_->GetDescriptor()->map_value();
    switch (field_descriptor->cpp_type()) {
      case FieldDescriptor::CPPTYPE_INT32: {
        map_val->SetInt32Value(other_it->second.GetInt32Value());
        break;
      }
      case FieldDescriptor::CPPTYPE_INT64: {
        map_val->SetInt64Value(other_it->second.GetInt64Value());
        break;
      }
      case FieldDescriptor::CPPTYPE_UINT32: {
        map_val->SetUInt32Value(other_it->second.GetUInt32Value());
        break;
      }
      case FieldDescriptor::CPPTYPE_UINT64: {
        map_val->SetUInt64Value(other_it->second.GetUInt64Value());
        break;
      }
      case FieldDescriptor::CPPTYPE_FLOAT: {
        map_val->SetFloatValue(other_it->second.GetFloatValue());
        break;
      }
      case FieldDescriptor::CPPTYPE_DOUBLE: {
        map_val->SetDoubleValue(other_it->second.GetDoubleValue());
        break;
      }
      case FieldDescriptor::CPPTYPE_BOOL: {
        map_val->SetBoolValue(other_it->second.GetBoolValue());
        break;
      }
      case FieldDescriptor::CPPTYPE_STRING: {
        map_val->SetStringValue(other_it->second.GetStringValue());
        break;
      }
      case FieldDescriptor::CPPTYPE_ENUM: {
        map_val->SetEnumValue(other_it->second.GetEnumValue());
        break;
      }
      case FieldDescriptor::CPPTYPE_MESSAGE: {
        map_val->MutableMessageValue()->CopyFrom(
            other_it->second.GetMessageValue());
        break;
      }
    }
  }
}

void DynamicMapField::SyncRepeatedFieldWithMapNoLock() const {
  const Reflection* reflection = default_entry_->GetReflection();
  const FieldDescriptor* key_des = default_entry_->GetDescriptor()->map_key();
  const FieldDescriptor* val_des = default_entry_->GetDescriptor()->map_value();

  auto& rep = payload().repeated_field;
  rep.Clear();

  for (Map<MapKey, MapValueRef>::const_iterator it = map_.begin();
       it != map_.end(); ++it) {
    Message* new_entry = default_entry_->New(arena());
    rep.AddAllocated(new_entry);
    const MapKey& map_key = it->first;
    switch (key_des->cpp_type()) {
      case FieldDescriptor::CPPTYPE_STRING:
        reflection->SetString(new_entry, key_des, map_key.GetStringValue());
        break;
      case FieldDescriptor::CPPTYPE_INT64:
        reflection->SetInt64(new_entry, key_des, map_key.GetInt64Value());
        break;
      case FieldDescriptor::CPPTYPE_INT32:
        reflection->SetInt32(new_entry, key_des, map_key.GetInt32Value());
        break;
      case FieldDescriptor::CPPTYPE_UINT64:
        reflection->SetUInt64(new_entry, key_des, map_key.GetUInt64Value());
        break;
      case FieldDescriptor::CPPTYPE_UINT32:
        reflection->SetUInt32(new_entry, key_des, map_key.GetUInt32Value());
        break;
      case FieldDescriptor::CPPTYPE_BOOL:
        reflection->SetBool(new_entry, key_des, map_key.GetBoolValue());
        break;
      case FieldDescriptor::CPPTYPE_DOUBLE:
      case FieldDescriptor::CPPTYPE_FLOAT:
      case FieldDescriptor::CPPTYPE_ENUM:
      case FieldDescriptor::CPPTYPE_MESSAGE:
        ABSL_LOG(FATAL) << "Can't get here.";
        break;
    }
    const MapValueRef& map_val = it->second;
    switch (val_des->cpp_type()) {
      case FieldDescriptor::CPPTYPE_STRING:
        reflection->SetString(new_entry, val_des, map_val.GetStringValue());
        break;
      case FieldDescriptor::CPPTYPE_INT64:
        reflection->SetInt64(new_entry, val_des, map_val.GetInt64Value());
        break;
      case FieldDescriptor::CPPTYPE_INT32:
        reflection->SetInt32(new_entry, val_des, map_val.GetInt32Value());
        break;
      case FieldDescriptor::CPPTYPE_UINT64:
        reflection->SetUInt64(new_entry, val_des, map_val.GetUInt64Value());
        break;
      case FieldDescriptor::CPPTYPE_UINT32:
        reflection->SetUInt32(new_entry, val_des, map_val.GetUInt32Value());
        break;
      case FieldDescriptor::CPPTYPE_BOOL:
        reflection->SetBool(new_entry, val_des, map_val.GetBoolValue());
        break;
      case FieldDescriptor::CPPTYPE_DOUBLE:
        reflection->SetDouble(new_entry, val_des, map_val.GetDoubleValue());
        break;
      case FieldDescriptor::CPPTYPE_FLOAT:
        reflection->SetFloat(new_entry, val_des, map_val.GetFloatValue());
        break;
      case FieldDescriptor::CPPTYPE_ENUM:
        reflection->SetEnumValue(new_entry, val_des, map_val.GetEnumValue());
        break;
      case FieldDescriptor::CPPTYPE_MESSAGE: {
        const Message& message = map_val.GetMessageValue();
        reflection->MutableMessage(new_entry, val_des)->CopyFrom(message);
        break;
      }
    }
  }
}

void DynamicMapField::SyncMapWithRepeatedFieldNoLock() const {
  Map<MapKey, MapValueRef>* map = &const_cast<DynamicMapField*>(this)->map_;
  const Reflection* reflection = default_entry_->GetReflection();
  const FieldDescriptor* key_des = default_entry_->GetDescriptor()->map_key();
  const FieldDescriptor* val_des = default_entry_->GetDescriptor()->map_value();
  Arena* arena = this->arena();
  // DynamicMapField owns map values. Need to delete them before clearing
  // the map.
  if (arena == nullptr) {
    for (Map<MapKey, MapValueRef>::iterator iter = map->begin();
         iter != map->end(); ++iter) {
      iter->second.DeleteData();
    }
  }
  map->clear();
  auto& rep = payload().repeated_field;
  for (const Message& elem : rep) {
    // MapKey type will be set later.
    MapKey map_key;
    switch (key_des->cpp_type()) {
      case FieldDescriptor::CPPTYPE_STRING:
        map_key.SetStringValue(reflection->GetString(elem, key_des));
        break;
      case FieldDescriptor::CPPTYPE_INT64:
        map_key.SetInt64Value(reflection->GetInt64(elem, key_des));
        break;
      case FieldDescriptor::CPPTYPE_INT32:
        map_key.SetInt32Value(reflection->GetInt32(elem, key_des));
        break;
      case FieldDescriptor::CPPTYPE_UINT64:
        map_key.SetUInt64Value(reflection->GetUInt64(elem, key_des));
        break;
      case FieldDescriptor::CPPTYPE_UINT32:
        map_key.SetUInt32Value(reflection->GetUInt32(elem, key_des));
        break;
      case FieldDescriptor::CPPTYPE_BOOL:
        map_key.SetBoolValue(reflection->GetBool(elem, key_des));
        break;
      case FieldDescriptor::CPPTYPE_DOUBLE:
      case FieldDescriptor::CPPTYPE_FLOAT:
      case FieldDescriptor::CPPTYPE_ENUM:
      case FieldDescriptor::CPPTYPE_MESSAGE:
        ABSL_LOG(FATAL) << "Can't get here.";
        break;
    }

    if (arena == nullptr) {
      // Remove existing map value with same key.
      Map<MapKey, MapValueRef>::iterator iter = map->find(map_key);
      if (iter != map->end()) {
        iter->second.DeleteData();
      }
    }

    MapValueRef& map_val = (*map)[map_key];
    map_val.SetType(val_des->cpp_type());
    switch (val_des->cpp_type()) {
#define HANDLE_TYPE(CPPTYPE, TYPE, METHOD)           \
  case FieldDescriptor::CPPTYPE_##CPPTYPE: {         \
    auto* value = Arena::Create<TYPE>(arena);        \
    *value = reflection->Get##METHOD(elem, val_des); \
    map_val.SetValue(value);                         \
    break;                                           \
  }
      HANDLE_TYPE(INT32, int32_t, Int32);
      HANDLE_TYPE(INT64, int64_t, Int64);
      HANDLE_TYPE(UINT32, uint32_t, UInt32);
      HANDLE_TYPE(UINT64, uint64_t, UInt64);
      HANDLE_TYPE(DOUBLE, double, Double);
      HANDLE_TYPE(FLOAT, float, Float);
      HANDLE_TYPE(BOOL, bool, Bool);
      HANDLE_TYPE(STRING, std::string, String);
      HANDLE_TYPE(ENUM, int32_t, EnumValue);
#undef HANDLE_TYPE
      case FieldDescriptor::CPPTYPE_MESSAGE: {
        const Message& message = reflection->GetMessage(elem, val_des);
        Message* value = message.New(arena);
        value->CopyFrom(message);
        map_val.SetValue(value);
        break;
      }
    }
  }
}

size_t DynamicMapField::SpaceUsedExcludingSelfNoLock() const {
  size_t size = 0;
  if (auto* p = maybe_payload()) {
    size += p->repeated_field.SpaceUsedExcludingSelfLong();
  }
  size += sizeof(map_);
  size_t map_size = map_.size();
  if (map_size) {
    Map<MapKey, MapValueRef>::const_iterator it = map_.begin();
    size += sizeof(it->first) * map_size;
    size += sizeof(it->second) * map_size;
    // If key is string, add the allocated space.
    if (it->first.type() == FieldDescriptor::CPPTYPE_STRING) {
      size += sizeof(std::string) * map_size;
    }
    // Add the allocated space in MapValueRef.
    switch (it->second.type()) {
#define HANDLE_TYPE(CPPTYPE, TYPE)           \
  case FieldDescriptor::CPPTYPE_##CPPTYPE: { \
    size += sizeof(TYPE) * map_size;         \
    break;                                   \
  }
      HANDLE_TYPE(INT32, int32_t);
      HANDLE_TYPE(INT64, int64_t);
      HANDLE_TYPE(UINT32, uint32_t);
      HANDLE_TYPE(UINT64, uint64_t);
      HANDLE_TYPE(DOUBLE, double);
      HANDLE_TYPE(FLOAT, float);
      HANDLE_TYPE(BOOL, bool);
      HANDLE_TYPE(STRING, std::string);
      HANDLE_TYPE(ENUM, int32_t);
#undef HANDLE_TYPE
      case FieldDescriptor::CPPTYPE_MESSAGE: {
        while (it != map_.end()) {
          const Message& message = it->second.GetMessageValue();
          size += message.GetReflection()->SpaceUsedLong(message);
          ++it;
        }
        break;
      }
    }
  }
  return size;
}

}  // namespace internal
}  // namespace protobuf
}  // namespace google

#include "google/protobuf/port_undef.inc"
