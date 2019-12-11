//===--- TypeJoinMeet.cpp - Swift Type "join" and "meet"  -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the "join" operation for types (and, eventually,
//  "meet").
//
//===----------------------------------------------------------------------===//

#include "polarphp/ast/AstContext.h"
#include "polarphp/ast/CanTypeVisitor.h"
#include "polarphp/ast/Decl.h"
#include "polarphp/ast/Type.h"
#include "polarphp/ast/Types.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace polar::ast {

namespace {

// FIXME: This is currently woefully incomplete, and is only currently
// used for optimizing away extra exploratory work in the constraint
// solver. It should eventually encompass all of the subtyping rules
// of the language.
struct TypeJoin : CanTypeVisitor<TypeJoin, CanType> {
   // The type we're joining with another type (the latter of which is
   // passed as an argument in the visitor.
   CanType First;

   // Always null. Used as a marker for places where we can improve the
   // implementation.
   CanType Unimplemented;

   // Always null. Used as a marker for places where there is no join
   // of two types in our type system.
   CanType Nonexistent;

   // For convenience, TheAnyType from ASTContext;
   CanType TheAnyType;

   TypeJoin(CanType First) : First(First), Unimplemented(CanType()) {
      assert(First && "Unexpected null type!");
      TheAnyType = First->getAstContext().TheAnyType;
   }

   static CanType getSuperclassJoin(CanType first, CanType second);

   CanType computeInterfaceCompositionJoin(ArrayRef<Type> firstMembers,
                                          ArrayRef<Type> secondMembers);


   CanType visitErrorType(CanType second);

   CanType visitTupleType(CanType second);

   CanType visitEnumType(CanType second);

   CanType visitStructType(CanType second);

   CanType visitClassType(CanType second);

   CanType visitInterfaceType(CanType second);

   CanType visitBoundGenericClassType(CanType second);

   CanType visitBoundGenericEnumType(CanType second);

   CanType visitBoundGenericStructType(CanType second);

   CanType visitMetatypeType(CanType second);

   CanType visitExistentialMetatypeType(CanType second);

   CanType visitModuleType(CanType second);

   CanType visitDynamicSelfType(CanType second);

   CanType visitArchetypeType(CanType second);

   CanType visitGenericTypeParamType(CanType second);

   CanType visitDependentMemberType(CanType second);

   CanType visitFunctionType(CanType second);

   CanType visitGenericFunctionType(CanType second);

   CanType visitInterfaceCompositionType(CanType second);

   CanType visitLValueType(CanType second);

   CanType visitInOutType(CanType second);

   CanType visitBuiltinType(CanType second);

   CanType visitType(CanType second) {
      return Unimplemented;
   }

public:
   static CanType join(CanType first, CanType second) {
      assert(!first->hasTypeVariable() && !second->hasTypeVariable() &&
             "Cannot compute join of types involving type variables");

      assert(first->getWithoutSpecifierType()->isEqual(first) &&
             "Expected simple type!");
      assert(second->getWithoutSpecifierType()->isEqual(second) &&
             "Expected simple type!");

      // If the types are equivalent, the join is obvious.
      if (first == second)
         return first;

      // Optionals broadly interact with all the other types since
      //   T <: T? for any T (including Any)
      // So we'll always attempt to dispatch Optional here rather than
      // make every visitor check for it explicitly.
      if (first->getOptionalObjectType())
         return TypeJoin(second).visit(first);

      if (second->getOptionalObjectType())
         return TypeJoin(first).visit(second);

      // Likewise, rather than making every visitor deal with Any,
      // always dispatch to the protocol composition side of the join.
      if (first->is<InterfaceCompositionType>())
         return TypeJoin(second).visit(first);

      if (second->is<InterfaceCompositionType>())
         return TypeJoin(first).visit(second);

      // Otherwise the first type might be an optional (or not), so
      // dispatch there.
      return TypeJoin(second).visit(first);
   }
};

CanType TypeJoin::getSuperclassJoin(CanType first, CanType second) {
   assert(first != second);

   // FIXME: Handle joins of classes and a single protocol?
   if (!first->mayHaveSuperclass() || !second->mayHaveSuperclass())
      return CanType();

   /// Walk the superclasses of `first` looking for `second`. Record them
   /// for our second step.
   llvm::SmallPtrSet<CanType, 8> superclassesOfFirst;
   for (Type super = first; super; super = super->getSuperclass()) {
      auto canSuper = super->getCanonicalType();

      // If we have found the second type, we're done.
      if (canSuper == second)
         return canSuper;

      superclassesOfFirst.insert(canSuper);
   }

   // Look through the superclasses of second to determine if any were also
   // superclasses of first.
   for (Type super = second; super; super = super->getSuperclass()) {
      auto canSuper = super->getCanonicalType();

      // If we found the first type, we're done.
      if (superclassesOfFirst.count(canSuper))
         return canSuper;
   }

   // FIXME: Unimplemented.
   return CanType();
}

CanType TypeJoin::visitErrorType(CanType second) {
   llvm_unreachable("join with ErrorType not supported");
   return second;
}

CanType TypeJoin::visitTupleType(CanType second) {
   assert(First != second);

   return TheAnyType;
}

CanType TypeJoin::visitEnumType(CanType second) {
   assert(First != second);

   return Unimplemented;
}

CanType TypeJoin::visitStructType(CanType second) {
   assert(First != second);

   // Deal with inout cases in visitInOutType.
   if (First->is<InOutType>())
      return TypeJoin(second).visit(First);

   // FIXME: When possible we should return a protocol or protocol
   // composition.
   return TheAnyType;
}

CanType TypeJoin::visitClassType(CanType second) {
   return getSuperclassJoin(First, second);
}

CanType TypeJoin::visitBoundGenericClassType(CanType second) {
   return getSuperclassJoin(First, second);
}

/// The subtype relationship of Optionals is as follows:
///   S  <: S?
///   S? <: T? if S <: T (covariant)
static Optional<CanType> joinOptional(CanType first, CanType second) {
   auto firstObject = first.getOptionalObjectType();
   auto secondObject = second.getOptionalObjectType();

   // If neither is any kind of Optional, we're done.
   if (!firstObject && !secondObject)
      return None;

   first = (firstObject ? firstObject : first);
   second = (secondObject ? secondObject : second);

   auto join = TypeJoin::join(first, second);
   if (!join)
      return None;

   return OptionalType::get(join)->getCanonicalType();
}

CanType TypeJoin::visitBoundGenericEnumType(CanType second) {
   // Deal with either First or second (or both) being optionals.
   if (auto joined = joinOptional(First, second))
      return joined.getValue();

   assert(First != second);

   return Unimplemented;
}

CanType TypeJoin::visitBoundGenericStructType(CanType second) {
   assert(First != second);

   // Deal with inout cases in visitInOutType.
   if (First->is<InOutType>())
      return TypeJoin(second).visit(First);

   return Unimplemented;
}

CanType TypeJoin::visitMetatypeType(CanType second) {
   assert(First != second);

   if (First->getKind() != second->getKind())
      return TheAnyType;

   auto firstInstance =
      First->castTo<AnyMetatypeType>()->getInstanceType()->getCanonicalType();
   auto secondInstance =
      second->castTo<AnyMetatypeType>()->getInstanceType()->getCanonicalType();

   auto joinInstance = join(firstInstance, secondInstance);
   if (!joinInstance)
      return CanType();

   return MetatypeType::get(joinInstance)->getCanonicalType();
}

CanType TypeJoin::visitExistentialMetatypeType(CanType second) {
   assert(First != second);

   if (First->getKind() != second->getKind())
      return TheAnyType;

   auto firstInstance =
      First->castTo<AnyMetatypeType>()->getInstanceType()->getCanonicalType();
   auto secondInstance =
      second->castTo<AnyMetatypeType>()->getInstanceType()->getCanonicalType();

   auto joinInstance = join(firstInstance, secondInstance);
   if (!joinInstance)
      return CanType();

   return ExistentialMetatypeType::get(joinInstance)->getCanonicalType();
}

CanType TypeJoin::visitModuleType(CanType second) {
   assert(First != second);

   return TheAnyType;
}

CanType TypeJoin::visitDynamicSelfType(CanType second) {
   return getSuperclassJoin(First, second);
}

CanType TypeJoin::visitArchetypeType(CanType second) {
   return getSuperclassJoin(First, second);
}

CanType TypeJoin::visitGenericTypeParamType(CanType second) {
   llvm_unreachable("Saw GenericTypeParamType in TypeJoin::join");
}

CanType TypeJoin::visitDependentMemberType(CanType second) {
   assert(First != second);

   if (First->getKind() != second->getKind())
      return TheAnyType;

   return Unimplemented;
}

CanType TypeJoin::visitFunctionType(CanType second) {
   assert(First != second);

   auto secondFnTy = second->castTo<FunctionType>();

   if (First->getKind() != second->getKind()) {
      if (secondFnTy->getExtInfo().isNoEscape()) {
         return Nonexistent;
      } else {
         return TheAnyType;
      }
   }

   auto firstFnTy = First->castTo<FunctionType>();

   auto firstExtInfo = firstFnTy->getExtInfo();
   auto secondExtInfo = secondFnTy->getExtInfo();

   // FIXME: Properly handle these attributes.
   if (firstExtInfo.withNoEscape(false) != secondExtInfo.withNoEscape(false))
      return Unimplemented;

   if (!AnyFunctionType::equalParams(firstFnTy->getParams(),
                                     secondFnTy->getParams()))
      return Unimplemented;

   auto firstResult = firstFnTy->getResult()->getCanonicalType();
   auto secondResult = secondFnTy->getResult()->getCanonicalType();

   auto result = join(firstResult, secondResult);
   if (!result)
      return Unimplemented;

   auto extInfo = firstExtInfo;
   if (secondFnTy->getExtInfo().isNoEscape())
      extInfo = extInfo.withNoEscape(true);

   return FunctionType::get(firstFnTy->getParams(), result, extInfo)
      ->getCanonicalType();
}

CanType TypeJoin::visitGenericFunctionType(CanType second) {
   assert(First != second);

   if (First->getKind() != second->getKind())
      return TheAnyType;

   return Unimplemented;
}

// Use the distributive law to compute the join of the protocol
// compositions.
//
//   (A ^ B) v (C ^ D)
// = (A v C) ^ (A v D) ^ (B v C) ^ (B v D)
//
// In general this law only applies to distributive lattices.
//
// In our case, this should be safe because our meet operation only
// produces an existing nominal type when it is one of the operands of
// the operation. So we can never arbitrarily climb down the lattice
// in ways that would break distributivity.
//
CanType TypeJoin::computeInterfaceCompositionJoin(ArrayRef<Type> firstMembers,
                                                 ArrayRef<Type> secondMembers) {
   SmallVector<Type, 8> result;
   for (auto first : firstMembers) {
      for (auto second : secondMembers) {
         auto joined = Type::join(first, second);
         if (!joined)
            return Unimplemented;

         if ((*joined)->isAny())
            continue;

         result.push_back(*joined);
      }
   }

   if (result.empty())
      return TheAnyType;

   auto &ctx = result[0]->getAstContext();
   return InterfaceCompositionType::get(ctx, result, false)->getCanonicalType();
}

CanType TypeJoin::visitInterfaceCompositionType(CanType second) {
   // The join of Any and a no-escape function doesn't exist; it isn't
   // Any. If it were Any, it would mean we would allow these functions
   // to escape through Any.
   if (second->isAny()) {
      auto *fnTy = First->getAs<AnyFunctionType>();
      if (fnTy && fnTy->getExtInfo().isNoEscape())
         return Nonexistent;

      return TheAnyType;
   }

   assert(First != second);

   // FIXME: Handle other types here.
   if (!First->isExistentialType())
      return Unimplemented;

   SmallVector<Type, 1> protocolType;
   ArrayRef<Type> firstMembers;
   if (First->is<InterfaceType>()) {
      protocolType.push_back(First);
      firstMembers = protocolType;
   } else {
      firstMembers = cast<InterfaceCompositionType>(First)->getMembers();
   }
   auto secondMembers = cast<InterfaceCompositionType>(second)->getMembers();

   return computeInterfaceCompositionJoin(firstMembers, secondMembers);
}

CanType TypeJoin::visitInterfaceType(CanType second) {
   assert(First != second);

   assert(!First->is<InterfaceCompositionType>() &&
          !second->is<InterfaceCompositionType>());

   // FIXME: Handle other types here.
   if (First->getKind() != second->getKind())
      return TheAnyType;

   auto *firstDecl =
      cast<InterfaceDecl>(First->getNominalOrBoundGenericNominal());

   auto *secondDecl =
      cast<InterfaceDecl>(second->getNominalOrBoundGenericNominal());

   if (firstDecl->getInheritedInterfaces().empty() &&
       secondDecl->getInheritedInterfaces().empty())
      return TheAnyType;

   if (firstDecl->inheritsFrom(secondDecl))
      return second;

   if (secondDecl->inheritsFrom(firstDecl))
      return First;

   // One isn't the supertype of the other, so instead, treat each as
   // if it's a protocol composition of its inherited members, and join
   // those.
   SmallVector<Type, 4> firstMembers;
   for (auto *decl : firstDecl->getInheritedInterfaces())
      firstMembers.push_back(decl->getDeclaredInterfaceType());

   SmallVector<Type, 4> secondMembers;
   for (auto *decl : secondDecl->getInheritedInterfaces())
      secondMembers.push_back(decl->getDeclaredInterfaceType());

   return computeInterfaceCompositionJoin(firstMembers, secondMembers);
}

CanType TypeJoin::visitLValueType(CanType second) { return Unimplemented; }

CanType TypeJoin::visitInOutType(CanType second) { return Unimplemented; }

CanType TypeJoin::visitBuiltinType(CanType second) {
   assert(First != second);

   // BuiltinType with any non-equal type results in Any.
   return TheAnyType;
}

} // namespace

Optional<Type> Type::join(Type first, Type second) {
   assert(first && second && "Unexpected null type!");

   if (!first || !second)
      return None;

   auto join =
      TypeJoin::join(first->getCanonicalType(), second->getCanonicalType());
   if (!join)
      return None;

   return join;
}

} // polar::ast