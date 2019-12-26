//===--- RuntimeFunctions.def - Runtime Functions Database ------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines x-macros used for metaprogramming with the set of
// runtime functions.
//
//===----------------------------------------------------------------------===//

/// FUNCTION(Id, Name, CC, Availability, ReturnTys, ArgTys, Attrs)
///   Makes available as "Id" the following runtime function:
///     ReturnTy Name(ArgTys...);
///   ReturnTys is a call to RETURNS, which takes a non-empty list
///     of expressions meant to be looked up in IRGenModule.
///   ArgTys is either NO_ARGS or a call to ARGS, which takes a non-empty
///     list of expressions meant to be looked up in IRGenModule.
///   Attrs is a parenthesized list of attributes.
///
///   By default, passes Id to FUNCTION_ID.  Therefore, the valid uses of
///   this database define either:
///     FUNCTION_ID
///   or all of the following:
///     FUNCTION
///     RETURNS
///     ARGS
///     NO_ARGS
///     ATTRS
///     NO_ATTRS
#ifndef FUNCTION
#define FUNCTION(Id, Name, CC, Availability, ReturnTys, ArgTys, Attrs) FUNCTION_ID(Id)
#endif

FUNCTION(AllocBox, polarphp_allocBox, SwiftCC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy, OpaquePtrTy),
         ARGS(TypeMetadataPtrTy),
         ATTRS(NoUnwind))

//  BoxPair polarphp_makeBoxUnique(OpaqueValue *buffer, Metadata *type, size_t alignMask);
FUNCTION(MakeBoxUnique,
         polarphp_makeBoxUnique,
         SwiftCC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy, OpaquePtrTy),
         ARGS(OpaquePtrTy, TypeMetadataPtrTy, SizeTy),
         ATTRS(NoUnwind))

FUNCTION(DeallocBox, polarphp_deallocBox, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind))

FUNCTION(ProjectBox, polarphp_projectBox, C_CC, AlwaysAvailable,
         RETURNS(OpaquePtrTy),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind, ReadNone))

FUNCTION(AllocEmptyBox, polarphp_allocEmptyBox, C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(),
         ATTRS(NoUnwind))

// RefCounted *polarphp_allocObject(Metadata *type, size_t size, size_t alignMask);
FUNCTION(AllocObject, polarphp_allocObject, C_CC,  AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(TypeMetadataPtrTy, SizeTy, SizeTy),
         ATTRS(NoUnwind))

// HeapObject *polarphp_initStackObject(HeapMetadata const *metadata,
//                                   HeapObject *object);
FUNCTION(InitStackObject, polarphp_initStackObject, C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(TypeMetadataPtrTy, RefCountedPtrTy),
         ATTRS(NoUnwind))

// HeapObject *polarphp_initStaticObject(HeapMetadata const *metadata,
//                                    HeapObject *object);
FUNCTION(InitStaticObject, polarphp_initStaticObject, C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(TypeMetadataPtrTy, RefCountedPtrTy),
         ATTRS(NoUnwind))

// void polarphp_verifyEndOfLifetime(HeapObject *object);
FUNCTION(VerifyEndOfLifetime, polarphp_verifyEndOfLifetime, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind))

// void polarphp_deallocObject(HeapObject *obj, size_t size, size_t alignMask);
FUNCTION(DeallocObject, polarphp_deallocObject, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy, SizeTy, SizeTy),
         ATTRS(NoUnwind))

// void polarphp_deallocUninitializedObject(HeapObject *obj, size_t size, size_t alignMask);
FUNCTION(DeallocUninitializedObject, polarphp_deallocUninitializedObject,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy, SizeTy, SizeTy),
         ATTRS(NoUnwind))

// void polarphp_deallocClassInstance(HeapObject *obj, size_t size, size_t alignMask);
FUNCTION(DeallocClassInstance, polarphp_deallocClassInstance, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy, SizeTy, SizeTy),
         ATTRS(NoUnwind))

// void polarphp_deallocPartialClassInstance(HeapObject *obj, HeapMetadata *type, size_t size, size_t alignMask);
FUNCTION(DeallocPartialClassInstance, polarphp_deallocPartialClassInstance,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy, TypeMetadataPtrTy, SizeTy, SizeTy),
         ATTRS(NoUnwind))

// void *polarphp_slowAlloc(size_t size, size_t alignMask);
FUNCTION(SlowAlloc, polarphp_slowAlloc, C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(SizeTy, SizeTy),
         ATTRS(NoUnwind))

// void polarphp_slowDealloc(void *ptr, size_t size, size_t alignMask);
FUNCTION(SlowDealloc, polarphp_slowDealloc, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(Int8PtrTy, SizeTy, SizeTy),
         ATTRS(NoUnwind))

// void polarphp_willThrow(error *ptr);
FUNCTION(WillThrow, polarphp_willThrow, SwiftCC,  AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(Int8PtrTy, ErrorPtrTy->getPointerTo()),
         ATTRS(NoUnwind))

// void polarphp_errorInMain(error *ptr);
FUNCTION(ErrorInMain, polarphp_errorInMain, SwiftCC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(ErrorPtrTy),
         ATTRS(NoUnwind))

// void polarphp_unexpectedError(error *ptr);
FUNCTION(UnexpectedError, polarphp_unexpectedError, SwiftCC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(ErrorPtrTy),
         ATTRS(NoUnwind, NoReturn))

// void *polarphp_copyPOD(void *dest, void *src, Metadata *self);
FUNCTION(CopyPOD, polarphp_copyPOD, C_CC, AlwaysAvailable,
         RETURNS(OpaquePtrTy),
         ARGS(OpaquePtrTy, OpaquePtrTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))

// void *polarphp_retain(void *ptr);
FUNCTION(NativeStrongRetain, polarphp_retain, C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_release(void *ptr);
FUNCTION(NativeStrongRelease, polarphp_release, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind))

// void *polarphp_retain_n(void *ptr, int32_t n);
FUNCTION(NativeStrongRetainN, polarphp_retain_n, C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(RefCountedPtrTy, Int32Ty),
         ATTRS(NoUnwind, FirstParamReturned))

// void *polarphp_release_n(void *ptr, int32_t n);
FUNCTION(NativeStrongReleaseN, polarphp_release_n, C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(RefCountedPtrTy, Int32Ty),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_setDeallocating(void *ptr);
FUNCTION(NativeSetDeallocating, polarphp_setDeallocating,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind))

// void *polarphp_nonatomic_retain_n(void *ptr, int32_t n);
FUNCTION(NativeNonAtomicStrongRetainN, polarphp_nonatomic_retain_n, C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(RefCountedPtrTy, Int32Ty),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_nonatomic_release_n(void *ptr, int32_t n);
FUNCTION(NativeNonAtomicStrongReleaseN, polarphp_nonatomic_release_n, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy, Int32Ty),
         ATTRS(NoUnwind))

// void *polarphp_unknownObjectRetain_n(void *ptr, int32_t n);
FUNCTION(UnknownObjectRetainN, polarphp_unknownObjectRetain_n,
         C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(RefCountedPtrTy, Int32Ty),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_unknownObjectRelease_n(void *ptr, int32_t n);
FUNCTION(UnknownObjectReleaseN, polarphp_unknownObjectRelease_n,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy, Int32Ty),
         ATTRS(NoUnwind))

// void *polarphp_nonatomic_unknownObjectRetain_n(void *ptr, int32_t n);
FUNCTION(NonAtomicUnknownObjectRetainN, polarphp_nonatomic_unknownObjectRetain_n,
         C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(RefCountedPtrTy, Int32Ty),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_nonatomic_unknownObjectRelease_n(void *ptr, int32_t n);
FUNCTION(NonAtomicUnknownObjectReleaseN, polarphp_nonatomic_unknownObjectRelease_n,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy, Int32Ty),
         ATTRS(NoUnwind))

// void polarphp_bridgeObjectRetain_n(void *ptr, int32_t n);
FUNCTION(BridgeObjectRetainN, polarphp_bridgeObjectRetain_n,
         C_CC, AlwaysAvailable,
         RETURNS(BridgeObjectPtrTy),
         ARGS(BridgeObjectPtrTy, Int32Ty),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_bridgeObjectRelease_n(void *ptr, int32_t n);
FUNCTION(BridgeObjectReleaseN, polarphp_bridgeObjectRelease_n,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(BridgeObjectPtrTy, Int32Ty),
         ATTRS(NoUnwind))

// void polarphp_nonatomic_bridgeObjectRetain_n(void *ptr, int32_t n);
FUNCTION(NonAtomicBridgeObjectRetainN, polarphp_nonatomic_bridgeObjectRetain_n,
         C_CC, AlwaysAvailable,
         RETURNS(BridgeObjectPtrTy),
         ARGS(BridgeObjectPtrTy, Int32Ty),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_nonatomic_bridgeObjectRelease_n(void *ptr, int32_t n);
FUNCTION(NonAtomicBridgeObjectReleaseN, polarphp_nonatomic_bridgeObjectRelease_n,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(BridgeObjectPtrTy, Int32Ty),
         ATTRS(NoUnwind))

// void *polarphp_nonatomic_retain(void *ptr);
FUNCTION(NativeNonAtomicStrongRetain, polarphp_nonatomic_retain,
         C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_nonatomic_release(void *ptr);
FUNCTION(NativeNonAtomicStrongRelease, polarphp_nonatomic_release,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind))

// void *polarphp_tryRetain(void *ptr);
FUNCTION(NativeTryRetain, polarphp_tryRetain, C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind))

// bool polarphp_isDeallocating(void *ptr);
FUNCTION(IsDeallocating, polarphp_isDeallocating, C_CC, AlwaysAvailable,
         RETURNS(Int1Ty),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind, ZExt))

// void *polarphp_unknownObjectRetain(void *ptr);
FUNCTION(UnknownObjectRetain, polarphp_unknownObjectRetain, C_CC, AlwaysAvailable,
         RETURNS(UnknownRefCountedPtrTy),
         ARGS(UnknownRefCountedPtrTy),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_unknownObjectRelease(void *ptr);
FUNCTION(UnknownObjectRelease, polarphp_unknownObjectRelease,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(UnknownRefCountedPtrTy),
         ATTRS(NoUnwind))

// void *polarphp_nonatomic_unknownObjectRetain(void *ptr);
FUNCTION(NonAtomicUnknownObjectRetain, polarphp_nonatomic_unknownObjectRetain,
         C_CC, AlwaysAvailable,
         RETURNS(UnknownRefCountedPtrTy),
         ARGS(UnknownRefCountedPtrTy),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_nonatomic_unknownObjectRelease(void *ptr);
FUNCTION(NonAtomicUnknownObjectRelease, polarphp_nonatomic_unknownObjectRelease,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(UnknownRefCountedPtrTy),
         ATTRS(NoUnwind))

// void *polarphp_bridgeObjectRetain(void *ptr);
FUNCTION(BridgeObjectStrongRetain, polarphp_bridgeObjectRetain,
         C_CC, AlwaysAvailable,
         RETURNS(BridgeObjectPtrTy),
         ARGS(BridgeObjectPtrTy),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_bridgeRelease(void *ptr);
FUNCTION(BridgeObjectStrongRelease, polarphp_bridgeObjectRelease,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(BridgeObjectPtrTy),
         ATTRS(NoUnwind))

// void *polarphp_nonatomic_bridgeObjectRetain(void *ptr);
FUNCTION(NonAtomicBridgeObjectStrongRetain, polarphp_nonatomic_bridgeObjectRetain,
         C_CC, AlwaysAvailable,
         RETURNS(BridgeObjectPtrTy),
         ARGS(BridgeObjectPtrTy),
         ATTRS(NoUnwind, FirstParamReturned))

// void polarphp_nonatomic_bridgeRelease(void *ptr);
FUNCTION(NonAtomicBridgeObjectStrongRelease,
         polarphp_nonatomic_bridgeObjectRelease,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(BridgeObjectPtrTy),
         ATTRS(NoUnwind))


// error *polarphp_errorRetain(error *ptr);
FUNCTION(ErrorStrongRetain, polarphp_errorRetain,
         C_CC, AlwaysAvailable,
         RETURNS(ErrorPtrTy),
         ARGS(ErrorPtrTy),
         ATTRS(NoUnwind))

// void polarphp_errorRelease(void *ptr);
FUNCTION(ErrorStrongRelease, polarphp_errorRelease,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(ErrorPtrTy),
         ATTRS(NoUnwind))

#define NEVER_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, Nativeness, SymName, UnknownPrefix) \
  /* void polarphp_##SymName##Destroy(Name##Reference *object); */ \
  FUNCTION(Nativeness##Name##Destroy, polarphp_##SymName##Destroy, \
           C_CC, AlwaysAvailable, \
           RETURNS(VoidTy), \
           ARGS(Name##ReferencePtrTy), \
           ATTRS(NoUnwind)) \
  /* void polarphp_##SymName##Init(Name##Reference *object, void *value); */ \
  FUNCTION(Nativeness##Name##Init, polarphp_##SymName##Init, \
           C_CC, AlwaysAvailable, \
           RETURNS(Name##ReferencePtrTy), \
           ARGS(Name##ReferencePtrTy, UnknownPrefix##RefCountedPtrTy), \
           ATTRS(NoUnwind, FirstParamReturned)) \
  /* Name##Reference *polarphp_##SymName##Assign(Name##Reference *object, void *value); */ \
  FUNCTION(Nativeness##Name##Assign, polarphp_##SymName##Assign, \
           C_CC, AlwaysAvailable, \
           RETURNS(Name##ReferencePtrTy), \
           ARGS(Name##ReferencePtrTy, UnknownPrefix##RefCountedPtrTy), \
           ATTRS(NoUnwind, FirstParamReturned)) \
  /* void *polarphp_##SymName##Load(Name##Reference *object); */ \
  FUNCTION(Nativeness##Name##LoadStrong, polarphp_##SymName##LoadStrong, \
           C_CC, AlwaysAvailable, \
           RETURNS(UnknownPrefix##RefCountedPtrTy), \
           ARGS(Name##ReferencePtrTy), \
           ATTRS(NoUnwind)) \
  /* void *polarphp_##SymName##Take(Name##Reference *object); */ \
  FUNCTION(Nativeness##Name##TakeStrong, polarphp_##SymName##TakeStrong, \
           C_CC, AlwaysAvailable, \
           RETURNS(UnknownPrefix##RefCountedPtrTy), \
           ARGS(Name##ReferencePtrTy), \
           ATTRS(NoUnwind)) \
  /* Name##Reference *polarphp_##SymName##CopyInit(Name##Reference *dest, Name##Reference *src); */ \
  FUNCTION(Nativeness##Name##CopyInit, polarphp_##SymName##CopyInit, \
           C_CC, AlwaysAvailable, \
           RETURNS(Name##ReferencePtrTy), \
           ARGS(Name##ReferencePtrTy, Name##ReferencePtrTy), \
           ATTRS(NoUnwind, FirstParamReturned)) \
  /* void *polarphp_##SymName##TakeInit(Name##Reference *dest, Name##Reference *src); */ \
  FUNCTION(Nativeness##Name##TakeInit, polarphp_##SymName##TakeInit, \
           C_CC, AlwaysAvailable, \
           RETURNS(Name##ReferencePtrTy), \
           ARGS(Name##ReferencePtrTy, Name##ReferencePtrTy), \
           ATTRS(NoUnwind, FirstParamReturned)) \
  /* Name##Reference *polarphp_##SymName##CopyAssign(Name##Reference *dest, Name##Reference *src); */ \
  FUNCTION(Nativeness##Name##CopyAssign, polarphp_##SymName##CopyAssign, \
           C_CC, AlwaysAvailable, \
           RETURNS(Name##ReferencePtrTy), \
           ARGS(Name##ReferencePtrTy, Name##ReferencePtrTy), \
           ATTRS(NoUnwind, FirstParamReturned)) \
  /* Name##Reference *polarphp_##SymName##TakeAssign(Name##Reference *dest, Name##Reference *src); */ \
  FUNCTION(Nativeness##Name##TakeAssign, polarphp_##SymName##TakeAssign, \
           C_CC, AlwaysAvailable, \
           RETURNS(Name##ReferencePtrTy), \
           ARGS(Name##ReferencePtrTy, Name##ReferencePtrTy), \
           ATTRS(NoUnwind, FirstParamReturned))
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, Native, name, ) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, Unknown, unknownObject##Name, Unknown)
#define LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name, Prefix, prefix) \
  /* void *polarphp_##prefix##name##Retain(void *ptr); */ \
  FUNCTION(Prefix##Name##Retain, polarphp_##prefix##name##Retain, \
           C_CC, AlwaysAvailable, \
           RETURNS(RefCountedPtrTy), \
           ARGS(RefCountedPtrTy), \
           ATTRS(NoUnwind, FirstParamReturned)) \
  /* void polarphp_##prefix##name##Release(void *ptr); */ \
  FUNCTION(Prefix##Name##Release, polarphp_##prefix##name##Release, \
           C_CC, AlwaysAvailable, \
           RETURNS(VoidTy), \
           ARGS(RefCountedPtrTy), \
           ATTRS(NoUnwind)) \
  /* void *polarphp_##prefix##name##RetainStrong(void *ptr); */ \
  FUNCTION(Prefix##StrongRetain##Name, polarphp_##prefix##name##RetainStrong, \
           C_CC, AlwaysAvailable, \
           RETURNS(RefCountedPtrTy), \
           ARGS(RefCountedPtrTy), \
           ATTRS(NoUnwind, FirstParamReturned)) \
  /* void polarphp_##prefix##name##RetainStrongAndRelease(void *ptr); */ \
  FUNCTION(Prefix##StrongRetainAnd##Name##Release, \
           polarphp_##prefix##name##RetainStrongAndRelease, \
           C_CC, AlwaysAvailable, \
           RETURNS(VoidTy), \
           ARGS(RefCountedPtrTy), \
           ATTRS(NoUnwind))
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, Unknown, unknownObject##Name, Unknown) \
  LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name, Native, ) \
  LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name, NonAtomicNative, nonatomic_)
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name, Native, ) \
  LOADABLE_CHECKED_REF_STORAGE_HELPER(Name, name, NonAtomicNative, nonatomic_)
#include "polarphp/ast/ReferenceStorageDef.h"
#undef NEVER_LOADABLE_CHECKED_REF_STORAGE_HELPER
#undef LOADABLE_CHECKED_REF_STORAGE_HELPER

// bool polarphp_isUniquelyReferencedNonObjC(const void *);
FUNCTION(IsUniquelyReferencedNonObjC, polarphp_isUniquelyReferencedNonObjC,
         C_CC, AlwaysAvailable,
         RETURNS(Int1Ty),
         ARGS(UnknownRefCountedPtrTy),
         ATTRS(NoUnwind, ZExt))

// bool polarphp_isUniquelyReferencedNonObjC_nonNull(const void *);
FUNCTION(IsUniquelyReferencedNonObjC_nonNull,
         polarphp_isUniquelyReferencedNonObjC_nonNull,
         C_CC, AlwaysAvailable,
         RETURNS(Int1Ty),
         ARGS(UnknownRefCountedPtrTy),
         ATTRS(NoUnwind, ZExt))

// bool polarphp_isUniquelyReferencedNonObjC_nonNull_bridgeObject(
//   uintptr_t bits);
FUNCTION(IsUniquelyReferencedNonObjC_nonNull_bridgeObject,
         polarphp_isUniquelyReferencedNonObjC_nonNull_bridgeObject,
         C_CC, AlwaysAvailable,
         RETURNS(Int1Ty),
         ARGS(BridgeObjectPtrTy),
         ATTRS(NoUnwind, ZExt))

// bool polarphp_isUniquelyReferenced_native(const struct HeapObject *);
FUNCTION(IsUniquelyReferenced_native, polarphp_isUniquelyReferenced_native,
         C_CC, AlwaysAvailable,
         RETURNS(Int1Ty),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind, ZExt))

// bool polarphp_isUniquelyReferenced_nonNull_native(const struct HeapObject *);
FUNCTION(IsUniquelyReferenced_nonNull_native,
         polarphp_isUniquelyReferenced_nonNull_native,
         C_CC, AlwaysAvailable,
         RETURNS(Int1Ty),
         ARGS(RefCountedPtrTy),
         ATTRS(NoUnwind, ZExt))

// bool polarphp_isEscapingClosureAtFileLocation(const struct HeapObject *object,
//                                            const unsigned char *filename,
//                                            int32_t filenameLength,
//                                            int32_t line,
//                                            int32_t col,
//                                            unsigned type);
FUNCTION(IsEscapingClosureAtFileLocation, polarphp_isEscapingClosureAtFileLocation,
         C_CC, AlwaysAvailable,
         RETURNS(Int1Ty),
         ARGS(RefCountedPtrTy, Int8PtrTy, Int32Ty, Int32Ty, Int32Ty, Int32Ty),
         ATTRS(NoUnwind, ZExt))

// void polarphp_arrayInitWithCopy(opaque*, opaque*, size_t, type*);
FUNCTION(ArrayInitWithCopy, polarphp_arrayInitWithCopy,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, OpaquePtrTy, SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))

// void polarphp_arrayInitWithTakeNoAlias(opaque*, opaque*, size_t, type*);
FUNCTION(ArrayInitWithTakeNoAlias, polarphp_arrayInitWithTakeNoAlias,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, OpaquePtrTy, SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))

// void polarphp_arrayInitWithTakeFrontToBack(opaque*, opaque*, size_t, type*);
FUNCTION(ArrayInitWithTakeFrontToBack, polarphp_arrayInitWithTakeFrontToBack,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, OpaquePtrTy, SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))

// void polarphp_arrayInitWithTakeBackToFront(opaque*, opaque*, size_t, type*);
FUNCTION(ArrayInitWithTakeBackToFront, polarphp_arrayInitWithTakeBackToFront,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, OpaquePtrTy, SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))

// void polarphp_arrayAssignWithCopyNoAlias(opaque*, opaque*, size_t, type*);
FUNCTION(ArrayAssignWithCopyNoAlias, polarphp_arrayAssignWithCopyNoAlias,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, OpaquePtrTy, SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))

// void polarphp_arrayAssignWithCopyFrontToBack(opaque*, opaque*, size_t, type*);
FUNCTION(ArrayAssignWithCopyFrontToBack, polarphp_arrayAssignWithCopyFrontToBack,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, OpaquePtrTy, SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))

// void polarphp_arrayAssignWithCopyBackToFront(opaque*, opaque*, size_t, type*);
FUNCTION(ArrayAssignWithCopyBackToFront, polarphp_arrayAssignWithCopyBackToFront,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, OpaquePtrTy, SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))

// void polarphp_arrayAssignWithTake(opaque*, opaque*, size_t, type*);
FUNCTION(ArrayAssignWithTake, polarphp_arrayAssignWithTake, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, OpaquePtrTy, SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))

// void polarphp_arrayDestroy(opaque*, size_t, type*);
FUNCTION(ArrayDestroy, polarphp_arrayDestroy, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))

// Metadata *polarphp_getFunctionTypeMetadata(unsigned long flags,
//                                         const Metadata **parameters,
//                                         const uint32_t *parameterFlags,
//                                         const Metadata *result);
FUNCTION(GetFunctionMetadata, polarphp_getFunctionTypeMetadata,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(SizeTy,
              TypeMetadataPtrTy->getPointerTo(0),
              Int32Ty->getPointerTo(0),
              TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadOnly))

// Metadata *polarphp_getFunctionTypeMetadata0(unsigned long flags,
//                                          const Metadata *resultMetadata);
FUNCTION(GetFunctionMetadata0, polarphp_getFunctionTypeMetadata0,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadNone))

// Metadata *polarphp_getFunctionTypeMetadata1(unsigned long flags,
//                                          const Metadata *arg0,
//                                          const Metadata *resultMetadata);
FUNCTION(GetFunctionMetadata1, polarphp_getFunctionTypeMetadata1,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(SizeTy, TypeMetadataPtrTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadNone))

// Metadata *polarphp_getFunctionTypeMetadata2(unsigned long flags,
//                                          const Metadata *arg0,
//                                          const Metadata *arg1,
//                                          const Metadata *resultMetadata);
FUNCTION(GetFunctionMetadata2, polarphp_getFunctionTypeMetadata2,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(SizeTy, TypeMetadataPtrTy, TypeMetadataPtrTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadNone))

// Metadata *polarphp_getFunctionTypeMetadata3(unsigned long flags,
//                                          const Metadata *arg0,
//                                          const Metadata *arg1,
//                                          const Metadata *arg2,
//                                          const Metadata *resultMetadata);
FUNCTION(GetFunctionMetadata3, polarphp_getFunctionTypeMetadata3,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(SizeTy, TypeMetadataPtrTy, TypeMetadataPtrTy, TypeMetadataPtrTy,
              TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadNone))

// Metadata *polarphp_getForeignTypeMetadata(Metadata *nonUnique);
FUNCTION(GetForeignTypeMetadata, polarphp_getForeignTypeMetadata,
         SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataResponseTy),
         ARGS(SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadNone)) // only writes to runtime-private fields

// MetadataResponse polarphp_getSingletonMetadata(MetadataRequest request,
//                                             TypeContextDescriptor *type);
FUNCTION(GetSingletonMetadata, polarphp_getSingletonMetadata,
         SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataResponseTy),
         ARGS(SizeTy, TypeContextDescriptorPtrTy),
         ATTRS(NoUnwind, ReadNone))

// MetadataResponse polarphp_getGenericMetadata(MetadataRequest request,
//                                           const void * const *arguments,
//                                           TypeContextDescriptor *type);
FUNCTION(GetGenericMetadata, polarphp_getGenericMetadata,
         SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataResponseTy),
         ARGS(SizeTy, Int8PtrTy, TypeContextDescriptorPtrTy),
         ATTRS(NoUnwind, ReadOnly))

// MetadataResponse polarphp_getOpaqueTypeMetadata(MetadataRequest request,
//                                     const void * const *arguments,
//                                     const OpaqueTypeDescriptor *descriptor,
//                                     uintptr_t index);
FUNCTION(GetOpaqueTypeMetadata, polarphp_getOpaqueTypeMetadata,
         SwiftCC, OpaqueTypeAvailability,
         RETURNS(TypeMetadataResponseTy),
         ARGS(SizeTy, Int8PtrTy, OpaqueTypeDescriptorPtrTy, SizeTy),
         ATTRS(NoUnwind, ReadOnly))

// const WitnessTable *polarphp_getOpaqueTypeConformance(const void * const *arguments,
//                                     const OpaqueTypeDescriptor *descriptor,
//                                     uintptr_t index);
FUNCTION(GetOpaqueTypeConformance, polarphp_getOpaqueTypeConformance,
         SwiftCC, OpaqueTypeAvailability,
         RETURNS(WitnessTablePtrTy),
         ARGS(Int8PtrTy, OpaqueTypeDescriptorPtrTy, SizeTy),
         ATTRS(NoUnwind, ReadOnly))

// Metadata *polarphp_allocateGenericClassMetadata(ClassDescriptor *type,
//                                              const void * const *arguments,
//                                              const void *template);
FUNCTION(AllocateGenericClassMetadata, polarphp_allocateGenericClassMetadata,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(TypeContextDescriptorPtrTy, Int8PtrPtrTy, Int8PtrTy),
         ATTRS(NoUnwind))

// Metadata *polarphp_allocateGenericValueMetadata(ValueTypeDescriptor *type,
//                                              const void * const *arguments,
//                                              const void *template,
//                                              size_t extraSize);
FUNCTION(AllocateGenericValueMetadata, polarphp_allocateGenericValueMetadata,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(TypeContextDescriptorPtrTy, Int8PtrPtrTy, Int8PtrTy, SizeTy),
         ATTRS(NoUnwind))

// MetadataResponse polarphp_checkMetadataState(MetadataRequest request,
//                                           const Metadata *type);
FUNCTION(CheckMetadataState, polarphp_checkMetadataState,
         SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataResponseTy),
         ARGS(SizeTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadOnly))

// const InterfaceWitnessTable *
// polarphp_getWitnessTable(const InterfaceConformanceDescriptor *conf,
//                       const Metadata *type,
//                       const void * const *instantiationArgs);
FUNCTION(GetWitnessTable, polarphp_getWitnessTable, C_CC, AlwaysAvailable,
         RETURNS(WitnessTablePtrTy),
         ARGS(InterfaceConformanceDescriptorPtrTy,
              TypeMetadataPtrTy,
              WitnessTablePtrPtrTy),
         ATTRS(NoUnwind, ReadOnly))

// MetadataResponse polarphp_getAssociatedTypeWitness(
//                                            MetadataRequest request,
//                                            WitnessTable *wtable,
//                                            const Metadata *conformingType,
//                                            InterfaceRequirement *reqBase,
//                                            InterfaceRequirement *assocType);
FUNCTION(GetAssociatedTypeWitness, polarphp_getAssociatedTypeWitness,
         SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataResponseTy),
         ARGS(SizeTy,
              WitnessTablePtrTy,
              TypeMetadataPtrTy,
              InterfaceRequirementStructTy->getPointerTo(),
              InterfaceRequirementStructTy->getPointerTo()),
         ATTRS(NoUnwind, ReadNone))

// SWIFT_RUNTIME_EXPORT SWIFT_CC(swift)
// const WitnessTable *polarphp_getAssociatedConformanceWitness(
//                                 WitnessTable *wtable,
//                                 const Metadata *conformingType,
//                                 const Metadata *assocType,
//                                 const InterfaceRequirement *reqBase,
//                                 const InterfaceRequirement *assocConformance);
FUNCTION(GetAssociatedConformanceWitness,
         polarphp_getAssociatedConformanceWitness, SwiftCC, AlwaysAvailable,
         RETURNS(WitnessTablePtrTy),
         ARGS(WitnessTablePtrTy,
              TypeMetadataPtrTy,
              TypeMetadataPtrTy,
              InterfaceRequirementStructTy->getPointerTo(),
              InterfaceRequirementStructTy->getPointerTo()),
         ATTRS(NoUnwind, ReadNone))

// Metadata *polarphp_getMetatypeMetadata(Metadata *instanceTy);
FUNCTION(GetMetatypeMetadata, polarphp_getMetatypeMetadata, C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadNone))

// Metadata *polarphp_getExistentialMetatypeMetadata(Metadata *instanceTy);
FUNCTION(GetExistentialMetatypeMetadata,
         polarphp_getExistentialMetatypeMetadata, C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadNone))

// Metadata *polarphp_getObjCClassMetadata(objc_class *theClass);
FUNCTION(GetObjCClassMetadata, polarphp_getObjCClassMetadata,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(ObjCClassPtrTy),
         ATTRS(NoUnwind, ReadNone))

// Metadata *polarphp_getObjCClassFromMetadata(objc_class *theClass);
FUNCTION(GetObjCClassFromMetadata, polarphp_getObjCClassFromMetadata,
         C_CC, AlwaysAvailable,
         RETURNS(ObjCClassPtrTy),
         ARGS(TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadNone))

// Metadata *polarphp_getObjCClassFromObject(id object);
FUNCTION(GetObjCClassFromObject, polarphp_getObjCClassFromObject,
         C_CC, AlwaysAvailable,
         RETURNS(ObjCClassPtrTy),
         ARGS(ObjCPtrTy),
         ATTRS(NoUnwind, ReadNone))

// MetadataResponse polarphp_getTupleTypeMetadata(MetadataRequest request,
//                                             TupleTypeFlags flags,
//                                             Metadata * const *elts,
//                                             const char *labels,
//                                             value_witness_table_t *proposed);
FUNCTION(GetTupleMetadata, polarphp_getTupleTypeMetadata, SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataResponseTy),
         ARGS(SizeTy, SizeTy, TypeMetadataPtrTy->getPointerTo(0),
              Int8PtrTy, WitnessTablePtrTy),
         ATTRS(NoUnwind, ReadOnly))

// MetadataResponse polarphp_getTupleTypeMetadata2(MetadataRequest request,
//                                              Metadata *elt0, Metadata *elt1,
//                                              const char *labels,
//                                              value_witness_table_t *proposed);
FUNCTION(GetTupleMetadata2, polarphp_getTupleTypeMetadata2, SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataResponseTy),
         ARGS(SizeTy, TypeMetadataPtrTy, TypeMetadataPtrTy,
              Int8PtrTy, WitnessTablePtrTy),
         ATTRS(NoUnwind, ReadOnly))

// MetadataResponse polarphp_getTupleTypeMetadata3(MetadataRequest request,
//                                              Metadata *elt0, Metadata *elt1,
//                                              Metadata *elt2,
//                                              const char *labels,
//                                              value_witness_table_t *proposed);
FUNCTION(GetTupleMetadata3, polarphp_getTupleTypeMetadata3, SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataResponseTy),
         ARGS(SizeTy, TypeMetadataPtrTy, TypeMetadataPtrTy, TypeMetadataPtrTy,
              Int8PtrTy, WitnessTablePtrTy),
         ATTRS(NoUnwind, ReadOnly))

// void polarphp_getTupleTypeLayout(TypeLayout *result,
//                               uint32_t offsets,
//                               TupleTypeFlags flags,
//                               const TypeLayout * const *elts);
FUNCTION(GetTupleLayout, polarphp_getTupleTypeLayout, SwiftCC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(FullTypeLayoutTy->getPointerTo(0), Int32Ty->getPointerTo(0),
              SizeTy, Int8PtrPtrTy->getPointerTo(0)),
         ATTRS(NoUnwind))

// size_t polarphp_getTupleTypeLayout2(TypeLayout *layout,
//                                  const TypeLayout *elt0,
//                                  const TypeLayout *elt1);
FUNCTION(GetTupleLayout2, polarphp_getTupleTypeLayout2, SwiftCC, AlwaysAvailable,
         RETURNS(SizeTy),
         ARGS(FullTypeLayoutTy->getPointerTo(0), Int8PtrPtrTy, Int8PtrPtrTy),
         ATTRS(NoUnwind))

// OffsetPair polarphp_getTupleTypeLayout3(TypeLayout *layout,
//                                      const TypeLayout *elt0,
//                                      const TypeLayout *elt1,
//                                      const TypeLayout *elt2);
FUNCTION(GetTupleLayout3, polarphp_getTupleTypeLayout3, SwiftCC, AlwaysAvailable,
         RETURNS(OffsetPairTy),
         ARGS(FullTypeLayoutTy->getPointerTo(0),
              Int8PtrPtrTy, Int8PtrPtrTy, Int8PtrPtrTy),
         ATTRS(NoUnwind))

// Metadata *polarphp_getExistentialTypeMetadata(
//                              InterfaceClassConstraint classConstraint,
//                              const Metadata *superclassConstraint,
//                              size_t numInterfaces,
//                              const InterfaceDescriptorRef *protocols);
//
// Note: InterfaceClassConstraint::Class is 0, ::Any is 1.
FUNCTION(GetExistentialMetadata,
         polarphp_getExistentialTypeMetadata,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(Int1Ty, TypeMetadataPtrTy, SizeTy,
              InterfaceDescriptorRefTy->getPointerTo()),
         ATTRS(NoUnwind, ReadOnly))

// Metadata *polarphp_relocateClassMetadata(TypeContextDescriptor *descriptor,
//                                       const void *pattern);
FUNCTION(RelocateClassMetadata,
         polarphp_relocateClassMetadata, C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(TypeContextDescriptorPtrTy, Int8PtrTy),
         ATTRS(NoUnwind))

// void polarphp_initClassMetadata(Metadata *self,
//                              ClassLayoutFlags flags,
//                              size_t numFields,
//                              TypeLayout * const *fieldTypes,
//                              size_t *fieldOffsets);
FUNCTION(InitClassMetadata,
         polarphp_initClassMetadata, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(TypeMetadataPtrTy, SizeTy, SizeTy,
              Int8PtrPtrTy->getPointerTo(),
              SizeTy->getPointerTo()),
         ATTRS(NoUnwind))

// void polarphp_updateClassMetadata(Metadata *self,
//                                ClassLayoutFlags flags,
//                                size_t numFields,
//                                TypeLayout * const *fieldTypes,
//                                size_t *fieldOffsets);
FUNCTION(UpdateClassMetadata,
         polarphp_updateClassMetadata, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(TypeMetadataPtrTy, SizeTy, SizeTy,
              Int8PtrPtrTy->getPointerTo(),
              SizeTy->getPointerTo()),
         ATTRS(NoUnwind))

// MetadataDependency polarphp_initClassMetadata2(Metadata *self,
//                                             ClassLayoutFlags flags,
//                                             size_t numFields,
//                                             TypeLayout * const *fieldTypes,
//                                             size_t *fieldOffsets);
FUNCTION(InitClassMetadata2,
         polarphp_initClassMetadata2, SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataDependencyTy),
         ARGS(TypeMetadataPtrTy, SizeTy, SizeTy,
              Int8PtrPtrTy->getPointerTo(),
              SizeTy->getPointerTo()),
         ATTRS(NoUnwind))

// MetadataDependency polarphp_updateClassMetadata2(Metadata *self,
//                                               ClassLayoutFlags flags,
//                                               size_t numFields,
//                                               TypeLayout * const *fieldTypes,
//                                               size_t *fieldOffsets);
FUNCTION(UpdateClassMetadata2,
         polarphp_updateClassMetadata2, SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataDependencyTy),
         ARGS(TypeMetadataPtrTy, SizeTy, SizeTy,
              Int8PtrPtrTy->getPointerTo(),
              SizeTy->getPointerTo()),
         ATTRS(NoUnwind))

// void *polarphp_lookUpClassMethod(Metadata *metadata,
//                               ClassDescriptor *description,
//                               MethodDescriptor *method);
FUNCTION(LookUpClassMethod,
         polarphp_lookUpClassMethod, C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(TypeMetadataPtrTy,
              MethodDescriptorStructTy->getPointerTo(),
              TypeContextDescriptorPtrTy),
         ATTRS(NoUnwind))

// void polarphp_initStructMetadata(Metadata *structType,
//                               StructLayoutFlags flags,
//                               size_t numFields,
//                               TypeLayout * const *fieldTypes,
//                               uint32_t *fieldOffsets);
FUNCTION(InitStructMetadata,
         polarphp_initStructMetadata, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(TypeMetadataPtrTy, SizeTy, SizeTy,
              Int8PtrPtrTy->getPointerTo(0),
              Int32Ty->getPointerTo()),
         ATTRS(NoUnwind))

// void polarphp_initEnumMetadataSingleCase(Metadata *enumType,
//                                       EnumLayoutFlags flags,
//                                       TypeLayout *payload);
FUNCTION(InitEnumMetadataSingleCase,
         polarphp_initEnumMetadataSingleCase,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(TypeMetadataPtrTy, SizeTy, Int8PtrPtrTy),
         ATTRS(NoUnwind))

// void polarphp_initEnumMetadataSinglePayload(Metadata *enumType,
//                                          EnumLayoutFlags flags,
//                                          TypeLayout *payload,
//                                          unsigned num_empty_cases);
FUNCTION(InitEnumMetadataSinglePayload,
         polarphp_initEnumMetadataSinglePayload,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(TypeMetadataPtrTy, SizeTy, Int8PtrPtrTy, Int32Ty),
         ATTRS(NoUnwind))

// void polarphp_initEnumMetadataMultiPayload(Metadata *enumType,
//                                         size_t numPayloads,
//                                         TypeLayout * const *payloadTypes);
FUNCTION(InitEnumMetadataMultiPayload,
         polarphp_initEnumMetadataMultiPayload,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(TypeMetadataPtrTy, SizeTy, SizeTy, Int8PtrPtrTy->getPointerTo(0)),
         ATTRS(NoUnwind))

// int polarphp_getEnumCaseMultiPayload(opaque_t *obj, Metadata *enumTy);
FUNCTION(GetEnumCaseMultiPayload,
         polarphp_getEnumCaseMultiPayload,
         C_CC, AlwaysAvailable,
         RETURNS(Int32Ty),
         ARGS(OpaquePtrTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadOnly))

// int polarphp_getEnumTagSinglePayloadGeneric(opaque_t *obj,
//                                          unsigned num_empty_cases,
//                                          Metadata *payloadType,
//                         int (*getExtraInhabitantIndex)(opaque_t *obj,
//                                                        unsigned numPayloadXI,
//                                                        Metadata *payload));
FUNCTION(GetEnumTagSinglePayloadGeneric,
         polarphp_getEnumTagSinglePayloadGeneric,
         SwiftCC, AlwaysAvailable,
         RETURNS(Int32Ty),
         ARGS(OpaquePtrTy, Int32Ty, TypeMetadataPtrTy,
              llvm::FunctionType::get(Int32Ty, {OpaquePtrTy, Int32Ty,
                                                TypeMetadataPtrTy},
                                      false)->getPointerTo()),
         ATTRS(NoUnwind, ReadOnly))


// void polarphp_storeEnumTagSinglePayloadGeneric(opaque_t *obj,
//                                             unsigned case_index,
//                                             unsigned num_empty_cases,
//                                             Metadata *payloadType,
//                           void (*storeExtraInhabitant)(opaque_t *obj,
//                                                        unsigned case_index,
//                                                        unsigned numPayloadXI,
//                                                        Metadata *payload));
FUNCTION(StoreEnumTagSinglePayloadGeneric,
         polarphp_storeEnumTagSinglePayloadGeneric,
         SwiftCC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, Int32Ty, Int32Ty, TypeMetadataPtrTy,
              llvm::FunctionType::get(VoidTy, {OpaquePtrTy, Int32Ty, Int32Ty,
                                               TypeMetadataPtrTy},
                                      false)->getPointerTo()),
         ATTRS(NoUnwind))

// void polarphp_storeEnumTagMultiPayload(opaque_t *obj, Metadata *enumTy,
//                                     int case_index);
FUNCTION(StoreEnumTagMultiPayload,
         polarphp_storeEnumTagMultiPayload,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OpaquePtrTy, TypeMetadataPtrTy, Int32Ty),
         ATTRS(NoUnwind))

// Class object_getClass(id object);
//
// This is readonly instead of readnone because isa-rewriting can have
// a noticeable effect.
FUNCTION(GetObjectClass, object_getClass, C_CC, AlwaysAvailable,
         RETURNS(ObjCClassPtrTy),
         ARGS(ObjCPtrTy),
         ATTRS(NoUnwind, ReadOnly))

// id object_dispose(id object);
FUNCTION(ObjectDispose, object_dispose, C_CC, AlwaysAvailable,
         RETURNS(ObjCPtrTy),
         ARGS(ObjCPtrTy),
         ATTRS(NoUnwind))

// Class objc_lookUpClass(const char *name);
FUNCTION(LookUpClass, objc_lookUpClass, C_CC, AlwaysAvailable,
         RETURNS(ObjCClassPtrTy),
         ARGS(Int8PtrTy),
         ATTRS(NoUnwind, ReadNone))

// Metadata *polarphp_getObjectType(id object);
FUNCTION(GetObjectType, polarphp_getObjectType, C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(ObjCPtrTy),
         ATTRS(NoUnwind, ReadOnly))

// Metadata *polarphp_getDynamicType(opaque_t *obj, Metadata *self);
FUNCTION(GetDynamicType, polarphp_getDynamicType, C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(OpaquePtrTy, TypeMetadataPtrTy, Int1Ty),
         ATTRS(NoUnwind, ReadOnly))

// void *polarphp_dynamicCastClass(void*, void*);
FUNCTION(DynamicCastClass, polarphp_dynamicCastClass, C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(Int8PtrTy, Int8PtrTy),
         ATTRS(NoUnwind, ReadOnly))

// void *polarphp_dynamicCastClassUnconditional(void*, void*);
FUNCTION(DynamicCastClassUnconditional, polarphp_dynamicCastClassUnconditional,
         C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(Int8PtrTy, Int8PtrTy, Int8PtrTy, Int32Ty, Int32Ty),
         ATTRS(NoUnwind, ReadOnly))

// void *polarphp_dynamicCastObjCClass(void*, void*);
FUNCTION(DynamicCastObjCClass, polarphp_dynamicCastObjCClass,
         C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(Int8PtrTy, Int8PtrTy),
         ATTRS(NoUnwind, ReadOnly))

// void *polarphp_dynamicCastObjCClassUnconditional(void*, void*);
FUNCTION(DynamicCastObjCClassUnconditional,
         polarphp_dynamicCastObjCClassUnconditional, C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(Int8PtrTy, Int8PtrTy, Int8PtrTy, Int32Ty, Int32Ty),
         ATTRS(NoUnwind, ReadOnly))

// void *polarphp_dynamicCastUnknownClass(void*, void*);
FUNCTION(DynamicCastUnknownClass, polarphp_dynamicCastUnknownClass,
         C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(Int8PtrTy, Int8PtrTy),
         ATTRS(NoUnwind, ReadOnly))

// void *polarphp_dynamicCastUnknownClassUnconditional(void*, void*);
FUNCTION(DynamicCastUnknownClassUnconditional,
         polarphp_dynamicCastUnknownClassUnconditional,
         C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(Int8PtrTy, Int8PtrTy, Int8PtrTy, Int32Ty, Int32Ty),
         ATTRS(NoUnwind, ReadOnly))

// type *polarphp_dynamicCastMetatype(type*, type*);
FUNCTION(DynamicCastMetatype, polarphp_dynamicCastMetatype,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(TypeMetadataPtrTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadOnly))

// type *polarphp_dynamicCastMetatypeUnconditional(type*, type*);
FUNCTION(DynamicCastMetatypeUnconditional,
         polarphp_dynamicCastMetatypeUnconditional,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(TypeMetadataPtrTy, TypeMetadataPtrTy, Int8PtrTy, Int32Ty, Int32Ty),
         ATTRS(NoUnwind, ReadOnly))

// objc_class *polarphp_dynamicCastObjCClassMetatype(objc_class*, objc_class*);
FUNCTION(DynamicCastObjCClassMetatype, polarphp_dynamicCastObjCClassMetatype,
         C_CC, AlwaysAvailable,
         RETURNS(ObjCClassPtrTy),
         ARGS(ObjCClassPtrTy, ObjCClassPtrTy),
         ATTRS(NoUnwind, ReadOnly))

// objc_class *polarphp_dynamicCastObjCClassMetatypeUnconditional(objc_class*, objc_class*);
FUNCTION(DynamicCastObjCClassMetatypeUnconditional,
         polarphp_dynamicCastObjCClassMetatypeUnconditional,
         C_CC, AlwaysAvailable,
         RETURNS(ObjCClassPtrTy),
         ARGS(ObjCClassPtrTy, ObjCClassPtrTy, Int8PtrTy, Int32Ty, Int32Ty),
         ATTRS(NoUnwind, ReadOnly))

// bool polarphp_dynamicCast(opaque*, opaque*, type*, type*, size_t);
FUNCTION(DynamicCast, polarphp_dynamicCast, C_CC, AlwaysAvailable,
         RETURNS(Int1Ty),
         ARGS(OpaquePtrTy, OpaquePtrTy, TypeMetadataPtrTy, TypeMetadataPtrTy,
              SizeTy),
         ATTRS(ZExt, NoUnwind))

// type* polarphp_dynamicCastTypeToObjCInterfaceUnconditional(type* object,
//                                               size_t numInterfaces,
//                                               Interface * const *protocols);
FUNCTION(DynamicCastTypeToObjCInterfaceUnconditional,
         polarphp_dynamicCastTypeToObjCInterfaceUnconditional,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(TypeMetadataPtrTy, SizeTy, Int8PtrPtrTy, Int8PtrTy, Int32Ty, Int32Ty),
         ATTRS(NoUnwind))

// type* polarphp_dynamicCastTypeToObjCInterfaceConditional(type* object,
//                                             size_t numInterfaces,
//                                             Interface * const *protocols);
FUNCTION(DynamicCastTypeToObjCInterfaceConditional,
         polarphp_dynamicCastTypeToObjCInterfaceConditional,
         C_CC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(TypeMetadataPtrTy, SizeTy, Int8PtrPtrTy),
         ATTRS(NoUnwind))

// id polarphp_dynamicCastObjCInterfaceUnconditional(id object,
//                                               size_t numInterfaces,
//                                               Interface * const *protocols);
FUNCTION(DynamicCastObjCInterfaceUnconditional,
         polarphp_dynamicCastObjCInterfaceUnconditional, C_CC, AlwaysAvailable,
         RETURNS(ObjCPtrTy),
         ARGS(ObjCPtrTy, SizeTy, Int8PtrPtrTy, Int8PtrTy, Int32Ty, Int32Ty),
         ATTRS(NoUnwind))

// id polarphp_dynamicCastObjCInterfaceConditional(id object,
//                                             size_t numInterfaces,
//                                             Interface * const *protocols);
FUNCTION(DynamicCastObjCInterfaceConditional,
         polarphp_dynamicCastObjCInterfaceConditional, C_CC, AlwaysAvailable,
         RETURNS(ObjCPtrTy),
         ARGS(ObjCPtrTy, SizeTy, Int8PtrPtrTy),
         ATTRS(NoUnwind))

// id polarphp_dynamicCastMetatypeToObjectUnconditional(type *type);
FUNCTION(DynamicCastMetatypeToObjectUnconditional,
         polarphp_dynamicCastMetatypeToObjectUnconditional, C_CC, AlwaysAvailable,
         RETURNS(ObjCPtrTy),
         ARGS(TypeMetadataPtrTy, Int8PtrTy, Int32Ty, Int32Ty),
         ATTRS(NoUnwind, ReadNone))

// id polarphp_dynamicCastMetatypeToObjectConditional(type *type);
FUNCTION(DynamicCastMetatypeToObjectConditional,
         polarphp_dynamicCastMetatypeToObjectConditional, C_CC, AlwaysAvailable,
         RETURNS(ObjCPtrTy),
         ARGS(TypeMetadataPtrTy),
         ATTRS(NoUnwind, ReadNone))

// witness_table* polarphp_conformsToInterface(type*, protocol*);
FUNCTION(ConformsToInterface,
         polarphp_conformsToInterface, C_CC, AlwaysAvailable,
         RETURNS(WitnessTablePtrTy),
         ARGS(TypeMetadataPtrTy, InterfaceDescriptorPtrTy),
         ATTRS(NoUnwind, ReadNone))

// bool polarphp_isClassType(type*);
FUNCTION(IsClassType,
         polarphp_isClassType, C_CC, AlwaysAvailable,
         RETURNS(Int1Ty),
         ARGS(TypeMetadataPtrTy),
         ATTRS(ZExt, NoUnwind, ReadNone))

// bool polarphp_isOptionalType(type*);
FUNCTION(IsOptionalType,
         polarphp_isOptionalType, C_CC, AlwaysAvailable,
         RETURNS(Int1Ty),
         ARGS(TypeMetadataPtrTy),
         ATTRS(ZExt, NoUnwind, ReadNone))

// void polarphp_once(polarphp_once_t *predicate,
//                 void (*function_code)(RefCounted*),
//                 void *context);
FUNCTION(Once, polarphp_once, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(OnceTy->getPointerTo(), Int8PtrTy, Int8PtrTy),
         ATTRS(NoUnwind))

// void polarphp_registerInterfaces(const InterfaceRecord *begin,
//                              const InterfaceRecord *end)
FUNCTION(RegisterInterfaces,
         polarphp_registerInterfaces, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(InterfaceRecordPtrTy, InterfaceRecordPtrTy),
         ATTRS(NoUnwind))

// void polarphp_registerInterfaceConformances(const InterfaceConformanceRecord *begin,
//                                         const InterfaceConformanceRecord *end)
FUNCTION(RegisterInterfaceConformances,
         polarphp_registerInterfaceConformances, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(RelativeAddressPtrTy, RelativeAddressPtrTy),
         ATTRS(NoUnwind))
FUNCTION(RegisterTypeMetadataRecords,
         polarphp_registerTypeMetadataRecords, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(TypeMetadataRecordPtrTy, TypeMetadataRecordPtrTy),
         ATTRS(NoUnwind))

// void polarphp_beginAccess(void *pointer, ValueBuffer *scratch, size_t flags);
FUNCTION(BeginAccess, polarphp_beginAccess, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(Int8PtrTy, getFixedBufferTy()->getPointerTo(), SizeTy, Int8PtrTy),
         ATTRS(NoUnwind))

// void polarphp_endAccess(ValueBuffer *scratch);
FUNCTION(EndAccess, polarphp_endAccess, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(getFixedBufferTy()->getPointerTo()),
         ATTRS(NoUnwind))

FUNCTION(GetOrigOfReplaceable, polarphp_getOrigOfReplaceable, C_CC,
         DynamicReplacementAvailability,
         RETURNS(FunctionPtrTy),
         ARGS(FunctionPtrTy->getPointerTo()),
         ATTRS(NoUnwind))

FUNCTION(GetReplacement, polarphp_getFunctionReplacement, C_CC,
         DynamicReplacementAvailability,
         RETURNS(FunctionPtrTy),
         ARGS(FunctionPtrTy->getPointerTo(), FunctionPtrTy),
         ATTRS(NoUnwind))

FUNCTION(InstantiateObjCClass, polarphp_instantiateObjCClass,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(TypeMetadataPtrTy),
         ATTRS(NoUnwind))
FUNCTION(ObjCAllocWithZone, objc_allocWithZone,
         C_CC, AlwaysAvailable,
         RETURNS(ObjCPtrTy), ARGS(ObjCClassPtrTy), ATTRS(NoUnwind))
FUNCTION(ObjCMsgSend, objc_msgSend,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy), NO_ARGS, NO_ATTRS)
FUNCTION(ObjCMsgSendStret, objc_msgSend_stret,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy), NO_ARGS, NO_ATTRS)
FUNCTION(ObjCMsgSendSuper, objc_msgSendSuper,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy), NO_ARGS, NO_ATTRS)
FUNCTION(ObjCMsgSendSuperStret, objc_msgSendSuper_stret,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy), NO_ARGS, NO_ATTRS)
FUNCTION(ObjCMsgSendSuper2, objc_msgSendSuper2,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy), NO_ARGS, NO_ATTRS)
FUNCTION(ObjCMsgSendSuperStret2, objc_msgSendSuper2_stret,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy), NO_ARGS, NO_ATTRS)
FUNCTION(ObjCSelRegisterName, sel_registerName,
         C_CC, AlwaysAvailable,
         RETURNS(ObjCSELTy), ARGS(Int8PtrTy), ATTRS(NoUnwind, ReadNone))
FUNCTION(ClassReplaceMethod, class_replaceMethod,
         C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(ObjCClassPtrTy, Int8PtrTy, Int8PtrTy, Int8PtrTy),
         ATTRS(NoUnwind))
FUNCTION(ClassAddInterface, class_addInterface,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(ObjCClassPtrTy, Int8PtrTy),
         ATTRS(NoUnwind))
FUNCTION(ObjCGetClass, objc_getClass, C_CC,  AlwaysAvailable,
         RETURNS(ObjCClassPtrTy),
         ARGS(Int8PtrTy),
         ATTRS(NoUnwind))
FUNCTION(ObjCGetMetaClass, objc_getMetaClass, C_CC, AlwaysAvailable,
         RETURNS(ObjCClassPtrTy),
         ARGS(Int8PtrTy),
         ATTRS(NoUnwind))
FUNCTION(ObjCClassGetName, class_getName, C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(ObjCClassPtrTy),
         ATTRS(NoUnwind))

FUNCTION(GetObjCInterface, objc_getInterface, C_CC, AlwaysAvailable,
         RETURNS(InterfaceDescriptorPtrTy),
         ARGS(Int8PtrTy),
         ATTRS(NoUnwind))
FUNCTION(AllocateObjCInterface, objc_allocateInterface, C_CC, AlwaysAvailable,
         RETURNS(InterfaceDescriptorPtrTy),
         ARGS(Int8PtrTy),
         ATTRS(NoUnwind))
FUNCTION(RegisterObjCInterface, objc_registerInterface, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(InterfaceDescriptorPtrTy),
         ATTRS(NoUnwind))
FUNCTION(InterfaceAddMethodDescription, protocol_addMethodDescription,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(InterfaceDescriptorPtrTy, Int8PtrTy, Int8PtrTy,
              ObjCBoolTy, ObjCBoolTy),
         ATTRS(NoUnwind))
FUNCTION(InterfaceAddInterface, protocol_addInterface,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(InterfaceDescriptorPtrTy, InterfaceDescriptorPtrTy),
         ATTRS(NoUnwind))

FUNCTION(Malloc, malloc, C_CC, AlwaysAvailable,
         RETURNS(Int8PtrTy),
         ARGS(SizeTy),
         NO_ATTRS)
FUNCTION(Free, free, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(Int8PtrTy),
         NO_ATTRS)

// void *_Block_copy(void *block);
FUNCTION(BlockCopy, _Block_copy, C_CC, AlwaysAvailable,
         RETURNS(ObjCBlockPtrTy),
         ARGS(ObjCBlockPtrTy),
         NO_ATTRS)
// void _Block_release(void *block);
FUNCTION(BlockRelease, _Block_release, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(ObjCBlockPtrTy),
         ATTRS(NoUnwind))

// void polarphp_deletedMethodError();
FUNCTION(DeletedMethodError, polarphp_deletedMethodError, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(),
         ATTRS(NoUnwind))

FUNCTION(AllocError, polarphp_allocError, SwiftCC, AlwaysAvailable,
         RETURNS(ErrorPtrTy, OpaquePtrTy),
         ARGS(TypeMetadataPtrTy, WitnessTablePtrTy, OpaquePtrTy, Int1Ty),
         ATTRS(NoUnwind))
FUNCTION(DeallocError, polarphp_deallocError, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(ErrorPtrTy, TypeMetadataPtrTy),
         ATTRS(NoUnwind))
FUNCTION(GetErrorValue, polarphp_getErrorValue, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(ErrorPtrTy, Int8PtrPtrTy, OpenedErrorTriplePtrTy),
         ATTRS(NoUnwind))

// void __tsan_external_write(void *addr, void *caller_pc, void *tag);
// This is a Thread Sanitizer instrumentation entry point in compiler-rt.
FUNCTION(TSanInoutAccess, __tsan_external_write, C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(Int8PtrTy, Int8PtrTy, Int8PtrTy),
         ATTRS(NoUnwind))

FUNCTION(GetKeyPath, polarphp_getKeyPath, C_CC, AlwaysAvailable,
         RETURNS(RefCountedPtrTy),
         ARGS(Int8PtrTy, Int8PtrTy),
         ATTRS(NoUnwind))
FUNCTION(CopyKeyPathTrivialIndices, polarphp_copyKeyPathTrivialIndices,
         C_CC,  AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(Int8PtrTy, Int8PtrTy, SizeTy),
         ATTRS(NoUnwind))

FUNCTION(GetInitializedObjCClass, polarphp_getInitializedObjCClass,
         C_CC, AlwaysAvailable,
         RETURNS(ObjCClassPtrTy),
         ARGS(ObjCClassPtrTy),
         ATTRS(NoUnwind))

// void polarphp_objc_swift3ImplicitObjCEntrypoint(id self, SEL selector)
FUNCTION(Swift3ImplicitObjCEntrypoint, polarphp_objc_swift3ImplicitObjCEntrypoint,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(ObjCPtrTy, ObjCSELTy, Int8PtrTy, SizeTy, SizeTy, SizeTy, Int8PtrTy),
         ATTRS(NoUnwind))

FUNCTION(VerifyTypeLayoutAttribute, _polarphp_debug_verifyTypeLayoutAttribute,
         C_CC, AlwaysAvailable,
         RETURNS(VoidTy),
         ARGS(TypeMetadataPtrTy, Int8PtrTy, Int8PtrTy, SizeTy, Int8PtrTy),
         ATTRS(NoUnwind))

// float polarphp_intToFloat32(const size_t *data, IntegerLiteralFlags flags);
FUNCTION(IntToFloat32, polarphp_intToFloat32, SwiftCC, AlwaysAvailable,
         RETURNS(FloatTy),
         ARGS(SizeTy->getPointerTo(), SizeTy),
         ATTRS(NoUnwind, ReadOnly))
FUNCTION(IntToFloat64, polarphp_intToFloat64, SwiftCC, AlwaysAvailable,
         RETURNS(DoubleTy),
         ARGS(SizeTy->getPointerTo(), SizeTy),
         ATTRS(NoUnwind, ReadOnly))

// const Metadata *polarphp_getTypeByMangledNameInContext(
//                        const char *typeNameStart,
//                        size_t typeNameLength,
//                        const TargetContextDescriptor<InProcess> *context,
//                        const void * const *genericArgs)
FUNCTION(GetTypeByMangledNameInContext, polarphp_getTypeByMangledNameInContext,
         SwiftCC, AlwaysAvailable,
         RETURNS(TypeMetadataPtrTy),
         ARGS(Int8PtrTy, SizeTy, TypeContextDescriptorPtrTy, Int8PtrPtrTy),
         ATTRS(NoUnwind, ArgMemOnly))

// const Metadata *polarphp_getTypeByMangledNameInContextInMetadataState(
//                        size_t metadataState,
//                        const char *typeNameStart,
//                        size_t typeNameLength,
//                        const TargetContextDescriptor<InProcess> *context,
//                        const void * const *genericArgs)
FUNCTION(GetTypeByMangledNameInContextInMetadataState,
         polarphp_getTypeByMangledNameInContextInMetadataState, SwiftCC,
         GetTypesInAbstractMetadataStateAvailability,
         RETURNS(TypeMetadataPtrTy),
         ARGS(SizeTy, Int8PtrTy, SizeTy, TypeContextDescriptorPtrTy,
              Int8PtrPtrTy),
         ATTRS(NoUnwind, ArgMemOnly))

#undef RETURNS
#undef ARGS
#undef ATTRS
#undef NO_ARGS
#undef NO_ATTRS
#undef FUNCTION
#undef FUNCTION_NAME
