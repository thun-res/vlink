// MIT License
//
// Copyright (c) 2018 Mindaugas Vinkelis
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef BITSERY_POINTER_UTILS_H
#define BITSERY_POINTER_UTILS_H

#include "polymorphism_utils.h"

namespace bitsery {
namespace ext {

// change name
enum class PointerType : uint8_t
{
  Nullable,
  NotNull
};

// Observer - not responsible for pointer lifetime management.
// Owner - only ONE owner is responsible for this pointers creation/destruction
// SharedOwner, SharedObserver - MANY shared owners is responsible for pointer
// creation/destruction requires additional context to manage shared owners
// themselves. SharedOwner actually manages life time e.g. std::shared_ptr
// SharedObserver do not manage life time of the pointer, but can observe shared
// state .e.. std::weak_ptr and differently from Observer, creates new object if
// necessary and saves to shared state
enum class PointerOwnershipType : uint8_t
{
  Observer,
  Owner,
  SharedOwner,
  SharedObserver
};

namespace pointer_utils {

// this class is used to store context for shared ptr owners
struct PointerSharedStateBase
{
  size_t typeId{};
  virtual ~PointerSharedStateBase() = default;
};

struct PointerSharedStateDeleter
{
  PointerSharedStateDeleter() = default;
  explicit PointerSharedStateDeleter(MemResourceBase* memResource)
    : _memResource{ memResource }
  {
  }
  void operator()(PointerSharedStateBase* data) const
  {
    data->~PointerSharedStateBase();
    StdPolyAlloc<PointerSharedStateBase> alloc{ _memResource };
    alloc.deallocate(data, 1);
  }
  MemResourceBase* _memResource = nullptr;
};

// PLC info is internal classes for serializer, and deserializer
struct PLCInfo
{
  explicit PLCInfo(PointerOwnershipType ownershipType_)
    : ownershipType{ ownershipType_ }
    , isSharedProcessed{ false } {};
  PointerOwnershipType ownershipType;
  bool isSharedProcessed;

  void update(PointerOwnershipType ptrType)
  {
    // do nothing for observer
    if (ptrType == PointerOwnershipType::Observer)
      return;
    if (ownershipType == PointerOwnershipType::Observer) {
      // set ownership type
      ownershipType = ptrType;
      return;
    }
    // only shared ownership can get here multiple times
    assert(ptrType == PointerOwnershipType::SharedOwner ||
           ptrType == PointerOwnershipType::SharedObserver);
    // check if need to update to SharedOwner
    if (ptrType == PointerOwnershipType::SharedOwner)
      ownershipType = ptrType;
    // mark that object already processed, so we do not serialize/deserialize
    // duplicate objects
    isSharedProcessed = true;
  }
};

struct PLCInfoSerializer : PLCInfo
{
  PLCInfoSerializer(size_t id_, PointerOwnershipType ownershipType_)
    : PLCInfo(ownershipType_)
    , id{ id_ }
  {
  }

  size_t id;
};

struct ObserverRef
{
  std::reference_wrapper<void*> obj;
  size_t baseTypeId;
};

struct PLCInfoDeserializer : PLCInfo
{
  PLCInfoDeserializer(void* ptr,
                      size_t ownerTypeId_,
                      PointerOwnershipType ownershipType_,
                      MemResourceBase* memResource_)
    : PLCInfo(ownershipType_)
    , ownerPtr{ ptr }
    , ownerTypeId{ ownerTypeId_ }
    , memResource{ memResource_ }
    , observersList{ StdPolyAlloc<ObserverRef>{ memResource_ } } {};

  // need to override these explicitly because we have pointer member
  PLCInfoDeserializer(const PLCInfoDeserializer&) = delete;

  PLCInfoDeserializer(PLCInfoDeserializer&&) = default;

  PLCInfoDeserializer& operator=(const PLCInfoDeserializer&) = delete;

  PLCInfoDeserializer& operator=(PLCInfoDeserializer&&) = default;

  void* ownerPtr;
  // used for polymorphic types in order to identify
  // if shared objects can be assigned
  size_t ownerTypeId;
  MemResourceBase* memResource;
  std::vector<ObserverRef, StdPolyAlloc<ObserverRef>> observersList;
  std::unique_ptr<PointerSharedStateBase, PointerSharedStateDeleter>
    sharedState{};
};

class PointerLinkingContextSerialization
{
public:
  explicit PointerLinkingContextSerialization(
    MemResourceBase* memResource = nullptr)
    : _currId{ 0 }
    , _ptrMap{ StdPolyAlloc<std::pair<const void* const, PLCInfoSerializer>>{
        memResource } }
  {
  }

  PointerLinkingContextSerialization(
    const PointerLinkingContextSerialization&) = delete;

  PointerLinkingContextSerialization& operator=(
    const PointerLinkingContextSerialization&) = delete;

  PointerLinkingContextSerialization(PointerLinkingContextSerialization&&) =
    default;

  PointerLinkingContextSerialization& operator=(
    PointerLinkingContextSerialization&&) = default;

  ~PointerLinkingContextSerialization() = default;

  const PLCInfoSerializer& getInfoByPtr(const void* ptr,
                                        PointerOwnershipType ptrType)
  {
    auto res = _ptrMap.emplace(ptr, PLCInfoSerializer{ _currId + 1u, ptrType });
    auto& ptrInfo = res.first->second;
    if (res.second) {
      ++_currId;
      return ptrInfo;
    }
    ptrInfo.update(ptrType);
    return ptrInfo;
  }

  // valid, when all pointers have owners.
  // we cannot serialize pointers, if we haven't serialized objects themselves
  bool isPointerSerializationValid() const
  {
    return std::all_of(
      _ptrMap.begin(),
      _ptrMap.end(),
      [](const std::pair<const void*, PLCInfoSerializer>& p) {
        return p.second.ownershipType == PointerOwnershipType::SharedOwner ||
               p.second.ownershipType == PointerOwnershipType::Owner;
      });
  }

private:
  size_t _currId;
  std::unordered_map<
    const void*,
    PLCInfoSerializer,
    std::hash<const void*>,
    std::equal_to<const void*>,
    StdPolyAlloc<std::pair<const void* const, PLCInfoSerializer>>>
    _ptrMap;
};

class PointerLinkingContextDeserialization
{
public:
  explicit PointerLinkingContextDeserialization(
    MemResourceBase* memResource = nullptr)
    : _memResource{ memResource }
    , _idMap{ StdPolyAlloc<std::pair<const size_t, PLCInfoDeserializer>>{
        memResource } }
  {
  }

  PointerLinkingContextDeserialization(
    const PointerLinkingContextDeserialization&) = delete;

  PointerLinkingContextDeserialization& operator=(
    const PointerLinkingContextDeserialization&) = delete;

  PointerLinkingContextDeserialization(PointerLinkingContextDeserialization&&) =
    default;

  PointerLinkingContextDeserialization& operator=(
    PointerLinkingContextDeserialization&&) = default;

  ~PointerLinkingContextDeserialization() = default;

  PLCInfoDeserializer& getInfoById(size_t id, PointerOwnershipType ptrType)
  {
    auto res = _idMap.emplace(
      id, PLCInfoDeserializer{ nullptr, 0, ptrType, _memResource });
    auto& ptrInfo = res.first->second;
    if (!res.second)
      ptrInfo.update(ptrType);
    return ptrInfo;
  }

  void clearSharedState()
  {
    for (auto& item : _idMap)
      item.second.sharedState.reset();
  }

  // valid, when all pointers has owners
  bool isPointerDeserializationValid() const
  {
    return std::all_of(
      _idMap.begin(),
      _idMap.end(),
      [](const std::pair<const size_t, PLCInfoDeserializer>& p) {
        return p.second.ownershipType == PointerOwnershipType::SharedOwner ||
               p.second.ownershipType == PointerOwnershipType::Owner;
      });
  }

  MemResourceBase* getMemResource() noexcept { return _memResource; }

  void setMemResource(MemResourceBase* resource) noexcept
  {
    _memResource = resource;
  }

private:
  MemResourceBase* _memResource;
  std::unordered_map<size_t,
                     PLCInfoDeserializer,
                     std::hash<size_t>,
                     std::equal_to<size_t>,
                     StdPolyAlloc<std::pair<const size_t, PLCInfoDeserializer>>>
    _idMap;
};
}

// this class is for convenience
class PointerLinkingContext
  : public pointer_utils::PointerLinkingContextSerialization
  , public pointer_utils::PointerLinkingContextDeserialization
{
public:
  explicit PointerLinkingContext(MemResourceBase* memResource = nullptr)
    : pointer_utils::PointerLinkingContextSerialization(memResource)
    , pointer_utils::PointerLinkingContextDeserialization(memResource){};

  bool isValid() const
  {
    return isPointerSerializationValid() && isPointerDeserializationValid();
  }
};

namespace pointer_utils {

template<template<typename> class TPtrManager,
         template<typename>
         class TPolymorphicContext,
         typename RTTI>
class PointerObjectExtensionBase
{
public:
  // helper types
  template<typename T>
  struct IsPolymorphic
    : std::integral_constant<
        bool,
        RTTI::template isPolymorphic<typename TPtrManager<T>::TElement>()>
  {};

  template<PointerOwnershipType Value>
  using OwnershipType = std::integral_constant<PointerOwnershipType, Value>;

  explicit PointerObjectExtensionBase(
    PointerType ptrType = PointerType::Nullable,
    MemResourceBase* resource = nullptr,
    bool resourcePropagate = false)
    : _ptrType{ ptrType }
    , _resourcePropagate{ resourcePropagate }
    , _resource{ resource }
  {
  }

  template<typename Ser, typename T, typename Fnc>
  void serialize(Ser& ser, const T& obj, Fnc&& fnc) const
  {

    auto ptr = TPtrManager<T>::getPtr(const_cast<T&>(obj));
    if (ptr) {
      auto& ctx = ser.template context<
        pointer_utils::PointerLinkingContextSerialization>();
      auto& ptrInfo = ctx.getInfoByPtr(getRootPtr(ser, ptr, IsPolymorphic<T>{}),
                                       TPtrManager<T>::getOwnership());
      details::writeSize(ser.adapter(), ptrInfo.id);
      if (TPtrManager<T>::getOwnership() != PointerOwnershipType::Observer) {
        if (!ptrInfo.isSharedProcessed)
          serializeImpl(ser, ptr, std::forward<Fnc>(fnc), IsPolymorphic<T>{});
      }
    } else {
      assert(_ptrType == PointerType::Nullable);
      details::writeSize(ser.adapter(), 0);
    }
  }

  template<typename Des, typename T, typename Fnc>
  void deserialize(Des& des, T& obj, Fnc&& fnc) const
  {
    size_t id{};
    details::readSize(des.adapter(), id, 0, std::false_type{});
    auto& ctx = des.template context<
      pointer_utils::PointerLinkingContextDeserialization>();
    auto prevResource = ctx.getMemResource();
    auto memResource = _resource ? _resource : prevResource;
    // if we have resource and propagate is true, then change current resource
    // so that deserializing nested pointers it will be used
    if (_resource && _resourcePropagate) {
      ctx.setMemResource(memResource);
    }
    if (id) {
      auto& ptrInfo = ctx.getInfoById(id, TPtrManager<T>::getOwnership());
      deserializeImpl(memResource,
                      ptrInfo,
                      des,
                      obj,
                      std::forward<Fnc>(fnc),
                      IsPolymorphic<T>{},
                      OwnershipType<TPtrManager<T>::getOwnership()>{});
    } else {
      if (_ptrType == PointerType::Nullable) {
        if (auto ptr = TPtrManager<T>::getPtr(obj)) {
          destroyPtr(memResource, des, obj, IsPolymorphic<T>{});
        };
      } else
        des.adapter().error(ReaderError::InvalidPointer);
    }
    if (_resource && _resourcePropagate) {
      ctx.setMemResource(prevResource);
    }
  }

private:
  template<typename Des, typename TObj>
  void destroyPtr(MemResourceBase* memResource,
                  Des& des,
                  TObj& obj,
                  std::true_type /*polymorphic*/) const
  {
    const auto& ctx = des.template context<TPolymorphicContext<RTTI>>();
    auto ptr = TPtrManager<TObj>::getPtr(obj);
    TPtrManager<TObj>::destroyPolymorphic(
      obj, memResource, ctx.getPolymorphicHandler(ptr));
  }

  template<typename Des, typename TObj>
  void destroyPtr(MemResourceBase* memResource,
                  Des&,
                  TObj& obj,
                  std::false_type /*polymorphic*/) const
  {
    TPtrManager<TObj>::destroy(
      obj,
      memResource,
      RTTI::template get<typename TPtrManager<TObj>::TElement>());
  }

  template<typename Ser, typename T>
  const void* getRootPtr(Ser&, const T* ptr, std::false_type) const
  {
    return ptr;
  }

  // same pointer can be accessed through different types with the same base
  // e.g. if we have Base class and Derived(Base) we'll get different pointer
  // address depending if we access it through Base or Derived
  // this function always returns "root" (Base) pointer.
  template<typename Ser, typename T>
  const void* getRootPtr(Ser& ser, const T* ptr, std::true_type) const
  {
    const auto& ctx = ser.template context<TPolymorphicContext<RTTI>>();
    return ctx.getPolymorphicHandler(ptr)->getRootPtr(ptr);
  }

  template<typename Ser, typename TPtr, typename Fnc>
  void serializeImpl(Ser& ser, TPtr& ptr, Fnc&&, std::true_type) const
  {
    const auto& ctx = ser.template context<TPolymorphicContext<RTTI>>();
    ctx.serialize(ser, *ptr);
  }

  template<typename Ser, typename TPtr, typename Fnc>
  void serializeImpl(Ser& ser, TPtr& ptr, Fnc&& fnc, std::false_type) const
  {
    fnc(ser, *ptr);
  }

  template<typename Des, typename T, typename Fnc>
  void deserializeImpl(MemResourceBase* memResource,
                       PLCInfoDeserializer& ptrInfo,
                       Des& des,
                       T& obj,
                       Fnc&&,
                       std::true_type,
                       OwnershipType<PointerOwnershipType::Owner>) const
  {
    const auto& ctx = des.template context<TPolymorphicContext<RTTI>>();
    ctx.deserialize(
      des,
      TPtrManager<T>::getPtr(obj),
      [&obj,
       memResource](const std::shared_ptr<PolymorphicHandlerBase>& handler) {
        TPtrManager<T>::createPolymorphic(obj, memResource, handler);
        return TPtrManager<T>::getPtr(obj);
      },
      [&obj,
       memResource](const std::shared_ptr<PolymorphicHandlerBase>& handler) {
        TPtrManager<T>::destroyPolymorphic(obj, memResource, handler);
      });
    auto ptr = TPtrManager<T>::getPtr(obj);
    // might be null in case data pointer is not valid
    if (ptr) {
      ptrInfo.ownerPtr = ptr;
      ptrInfo.ownerTypeId = ctx.getPolymorphicHandler(ptr)->getDerivedTypeId();
      processObserverListPolymorphic(des, ptrInfo, ctx);
    }
  }

  template<typename Des, typename T, typename Fnc>
  void deserializeImpl(MemResourceBase* memResource,
                       PLCInfoDeserializer& ptrInfo,
                       Des& des,
                       T& obj,
                       Fnc&& fnc,
                       std::false_type,
                       OwnershipType<PointerOwnershipType::Owner>) const
  {
    auto ptr = TPtrManager<T>::getPtr(obj);
    if (!ptr) {
      TPtrManager<T>::create(
        obj,
        memResource,
        RTTI::template get<typename TPtrManager<T>::TElement>());
      ptr = TPtrManager<T>::getPtr(obj);
    }
    fnc(des, *ptr);
    ptrInfo.ownerPtr = ptr;
    ptrInfo.ownerTypeId =
      RTTI::template get<typename TPtrManager<T>::TElement>();
    processObserverList(des, ptrInfo);
  }

  template<typename Des, typename T, typename Fnc>
  void deserializeImpl(MemResourceBase* memResource,
                       PLCInfoDeserializer& ptrInfo,
                       Des& des,
                       T& obj,
                       Fnc&&,
                       std::true_type,
                       OwnershipType<PointerOwnershipType::SharedOwner>) const
  {
    const auto& ctx = des.template context<TPolymorphicContext<RTTI>>();
    const size_t baseTypeId =
      RTTI::template get<typename TPtrManager<T>::TElement>();
    size_t deserializedTypeId = 0;
    if (!ptrInfo.sharedState) {
      ctx.deserialize(
        des,
        TPtrManager<T>::getPtr(obj),
        [&obj, &ptrInfo, memResource, this](
          const std::shared_ptr<PolymorphicHandlerBase>& handler) {
          TPtrManager<T>::createSharedPolymorphic(
            createAndGetSharedStateObj<T>(ptrInfo), obj, memResource, handler);
          return TPtrManager<T>::getPtr(obj);
        },
        [&obj,
         memResource](const std::shared_ptr<PolymorphicHandlerBase>& handler) {
          TPtrManager<T>::destroyPolymorphic(obj, memResource, handler);
        });
      if (!ptrInfo.sharedState) {
        TPtrManager<T>::saveToSharedStatePolymorphic(
          createAndGetSharedStateObj<T>(ptrInfo), obj);
      }
      ptrInfo.ownerPtr = TPtrManager<T>::getPtr(obj);
      ptrInfo.ownerTypeId =
        ctx.getPolymorphicHandler(TPtrManager<T>::getPtr(obj))
          ->getDerivedTypeId();
      // since we just deserialized an object, we can skip checking hierarchy
      // chain by assigning baseType id instead of derived type id
      deserializedTypeId = baseTypeId;
    } else {
      deserializedTypeId = ptrInfo.ownerTypeId;
    }
    if (auto hndl =
          ctx.getPolymorphicHandler(baseTypeId, ptrInfo.sharedState->typeId)) {
      TPtrManager<T>::loadFromSharedStatePolymorphic(
        getSharedStateObj<T>(ptrInfo), obj, **hndl);
      processObserverListPolymorphic(des, ptrInfo, ctx);
    } else {
      des.adapter().error(ReaderError::InvalidPointer);
    }
  }

  template<typename Des, typename T, typename Fnc>
  void deserializeImpl(MemResourceBase* memResource,
                       PLCInfoDeserializer& ptrInfo,
                       Des& des,
                       T& obj,
                       Fnc&& fnc,
                       std::false_type,
                       OwnershipType<PointerOwnershipType::SharedOwner>) const
  {
    const size_t baseTypeId =
      RTTI::template get<typename TPtrManager<T>::TElement>();
    if (!ptrInfo.sharedState) {
      auto ptr = TPtrManager<T>::getPtr(obj);
      if (ptr) {
        TPtrManager<T>::saveToSharedState(
          createAndGetSharedStateObj<T>(ptrInfo), obj);
      } else {
        TPtrManager<T>::createShared(
          createAndGetSharedStateObj<T>(ptrInfo),
          obj,
          memResource,
          RTTI::template get<typename TPtrManager<T>::TElement>());
        ptr = TPtrManager<T>::getPtr(obj);
      }
      fnc(des, *ptr);
      ptrInfo.ownerTypeId =
        RTTI::template get<typename TPtrManager<T>::TElement>();
      ptrInfo.ownerPtr = TPtrManager<T>::getPtr(obj);
    }
    if (baseTypeId == ptrInfo.ownerTypeId) {
      TPtrManager<T>::loadFromSharedState(getSharedStateObj<T>(ptrInfo), obj);
      processObserverList(des, ptrInfo);
    } else {
      des.adapter().error(ReaderError::InvalidPointer);
    }
  }

  template<typename Des, typename T, typename Fnc, typename isPolymorph>
  void deserializeImpl(
    MemResourceBase* memResource,
    PLCInfoDeserializer& ptrInfo,
    Des& des,
    T& obj,
    Fnc&& fnc,
    isPolymorph polymorph,
    OwnershipType<PointerOwnershipType::SharedObserver>) const
  {
    deserializeImpl(memResource,
                    ptrInfo,
                    des,
                    obj,
                    fnc,
                    polymorph,
                    OwnershipType<PointerOwnershipType::SharedOwner>{});
  }

  template<typename Des, typename T, typename Fnc>
  void deserializeImpl(MemResourceBase*,
                       PLCInfoDeserializer& ptrInfo,
                       Des& des,
                       T& obj,
                       Fnc&&,
                       std::false_type,
                       OwnershipType<PointerOwnershipType::Observer>) const
  {
    auto baseTypeId = RTTI::template get<typename TPtrManager<T>::TElement>();
    void*(&ptr) = reinterpret_cast<void*&>(TPtrManager<T>::getPtrRef(obj));
    if (ptrInfo.ownerPtr) {
      if (ptrInfo.ownerTypeId == baseTypeId) {
        ptr = ptrInfo.ownerPtr;
      } else {
        des.adapter().error(ReaderError::InvalidPointer);
      }
    } else {
      ptrInfo.observersList.emplace_back(ObserverRef{ ptr, baseTypeId });
    }
  }

  template<typename Des, typename T, typename Fnc>
  void deserializeImpl(MemResourceBase*,
                       PLCInfoDeserializer& ptrInfo,
                       Des& des,
                       T& obj,
                       Fnc&&,
                       std::true_type,
                       OwnershipType<PointerOwnershipType::Observer>) const
  {
    const auto& ctx = des.template context<TPolymorphicContext<RTTI>>();
    const size_t baseTypeId =
      RTTI::template get<typename TPtrManager<T>::TElement>();
    void*(&ptr) = reinterpret_cast<void*&>(TPtrManager<T>::getPtrRef(obj));
    if (ptrInfo.ownerPtr) {
      if (auto hndl =
            ctx.getPolymorphicHandler(baseTypeId, ptrInfo.ownerTypeId)) {
        ptr = hndl->get()->fromDerivedToBasePtr(ptrInfo.ownerPtr);
      } else {
        des.adapter().error(ReaderError::InvalidPointer);
      }
    } else {
      ptrInfo.observersList.emplace_back(ObserverRef{ ptr, baseTypeId });
    }
  }

  template<typename Des>
  void processObserverList(Des& des, PLCInfoDeserializer& ptrInfo) const
  {
    assert(ptrInfo.ownershipType != PointerOwnershipType::Observer);
    for (auto& o : ptrInfo.observersList) {
      if (ptrInfo.ownerTypeId == o.baseTypeId) {
        o.obj.get() = ptrInfo.ownerPtr;
      } else {
        des.adapter().error(ReaderError::InvalidPointer);
      }
    }
    ptrInfo.observersList.clear();
    ptrInfo.observersList.shrink_to_fit();
  }

  template<typename Des>
  void processObserverListPolymorphic(
    Des& des,
    PLCInfoDeserializer& ptrInfo,
    const TPolymorphicContext<RTTI>& ctx) const
  {
    assert(ptrInfo.ownershipType != PointerOwnershipType::Observer);
    for (auto& o : ptrInfo.observersList) {
      if (auto hndl =
            ctx.getPolymorphicHandler(o.baseTypeId, ptrInfo.ownerTypeId)) {
        o.obj.get() = hndl->get()->fromDerivedToBasePtr(ptrInfo.ownerPtr);
      } else {
        des.adapter().error(ReaderError::InvalidPointer);
      }
    }
    ptrInfo.observersList.clear();
    ptrInfo.observersList.shrink_to_fit();
  }

  template<typename T>
  typename TPtrManager<T>::TSharedState& createAndGetSharedStateObj(
    PLCInfoDeserializer& info) const
  {
    using TSharedState = typename TPtrManager<T>::TSharedState;
    StdPolyAlloc<TSharedState> alloc{ info.memResource };
    auto* ptr = alloc.allocate(1);
    auto* obj = new (ptr) TSharedState{};
    info.sharedState =
      std::unique_ptr<PointerSharedStateBase, PointerSharedStateDeleter>(
        obj, PointerSharedStateDeleter{ info.memResource });
    return *obj;
  }

  template<typename T>
  typename TPtrManager<T>::TSharedState& getSharedStateObj(
    PLCInfoDeserializer& info) const
  {
    return static_cast<typename TPtrManager<T>::TSharedState&>(
      *info.sharedState);
  }

  PointerType _ptrType;
  bool _resourcePropagate;
  MemResourceBase* _resource;
};

}

}
}

#endif // BITSERY_POINTER_UTILS_H
