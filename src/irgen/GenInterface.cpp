//===--- GenProto.cpp - Swift IR Generation for Interfaces -----------------===//
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
//  This file implements IR generation for protocols in Swift.
//
//  Interfaces serve two masters: generic algorithms and existential
//  types.  In either case, the size and structure of a type is opaque
//  to the code manipulating a value.  Local values of the type must
//  be stored in fixed-size buffers (which can overflow to use heap
//  allocation), and basic operations on the type must be dynamically
//  delegated to a collection of information that "witnesses" the
//  truth that a particular type implements the protocol.
//
//  In the comments throughout this file, three type names are used:
//    'B' is the type of a fixed-size buffer
//    'T' is the type which implements a protocol
//    'W' is the type of a witness to the protocol
//
//===----------------------------------------------------------------------===//

#include "polarphp/ast/AstContext.h"
#include "polarphp/ast/CanTypeVisitor.h"
#include "polarphp/ast/Types.h"
#include "polarphp/ast/Decl.h"
#include "polarphp/ast/GenericEnvironment.h"
#include "polarphp/ast/LazyResolver.h"
#include "polarphp/ast/IRGenOptions.h"
#include "polarphp/ast/PrettyStackTrace.h"
#include "polarphp/ast/SubstitutionMap.h"
#include "polarphp/clangimporter/ClangModule.h"
#include "polarphp/irgen/Linking.h"
#include "polarphp/pil/lang/PILDeclRef.h"
#include "polarphp/pil/lang/PILDefaultWitnessTable.h"
#include "polarphp/pil/lang/PILModule.h"
#include "polarphp/pil/lang/PILValue.h"
#include "polarphp/pil/lang/PILWitnessTable.h"
#include "polarphp/pil/lang/PILWitnessVisitor.h"
#include "polarphp/pil/lang/TypeLowering.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include "polarphp/irgen/internal/CallEmission.h"
#include "polarphp/irgen/internal/ConformanceDescription.h"
#include "polarphp/irgen/internal/ConstantBuilder.h"
#include "polarphp/irgen/internal/EnumPayload.h"
#include "polarphp/irgen/internal/Explosion.h"
#include "polarphp/irgen/internal/FixedTypeInfo.h"
#include "polarphp/irgen/internal/Fulfillment.h"
#include "polarphp/irgen/internal/GenArchetype.h"
#include "polarphp/irgen/internal/GenCast.h"
#include "polarphp/irgen/internal/GenClass.h"
#include "polarphp/irgen/internal/GenEnum.h"
#include "polarphp/irgen/internal/GenHeap.h"
#include "polarphp/irgen/internal/GenMeta.h"
#include "polarphp/irgen/internal/GenOpaque.h"
#include "polarphp/irgen/internal/GenPoly.h"
#include "polarphp/irgen/internal/GenType.h"
#include "polarphp/irgen/internal/GenericRequirement.h"
#include "polarphp/irgen/internal/IRGenDebugInfo.h"
#include "polarphp/irgen/internal/IRGenFunction.h"
#include "polarphp/irgen/internal/IRGenMangler.h"
#include "polarphp/irgen/internal/IRGenModule.h"
#include "polarphp/irgen/internal/MetadataPath.h"
#include "polarphp/irgen/internal/MetadataRequest.h"
#include "polarphp/irgen/internal/NecessaryBindings.h"
#include "polarphp/irgen/internal/InterfaceInfo.h"
#include "polarphp/irgen/internal/TypeInfo.h"

#include "polarphp/irgen/internal/GenInterface.h"

using namespace polar;
using namespace irgen;

namespace {

/// A class for computing how to pass arguments to a polymorphic
/// function.  The subclasses of this are the places which need to
/// be updated if the convention changes.
class PolymorphicConvention {
protected:
   IRGenModule &IGM;
   ModuleDecl &M;
   CanPILFunctionType FnType;

   CanGenericSignature Generics;

   std::vector<MetadataSource> Sources;

   FulfillmentMap Fulfillments;

   GenericSignature::ConformsToArray getConformsTo(Type t) {
      return Generics->getConformsTo(t);
   }

   CanType getSuperclassBound(Type t) {
      if (auto superclassTy = Generics->getSuperclassBound(t))
         return superclassTy->getCanonicalType();
      return CanType();
   }

public:
   PolymorphicConvention(IRGenModule &IGM, CanPILFunctionType fnType);

   ArrayRef<MetadataSource> getSources() const { return Sources; }

   void enumerateRequirements(const RequirementCallback &callback);

   void enumerateUnfulfilledRequirements(const RequirementCallback &callback);

   /// Returns a Fulfillment for a type parameter requirement, or
   /// nullptr if it's unfulfilled.
   const Fulfillment *getFulfillmentForTypeMetadata(CanType type) const;

   /// Return the source of type metadata at a particular source index.
   const MetadataSource &getSource(size_t SourceIndex) const {
      return Sources[SourceIndex];
   }

private:
   void initGenerics();
   void considerNewTypeSource(MetadataSource::Kind kind, unsigned paramIndex,
                              CanType type, IsExact_t isExact);
   bool considerType(CanType type, IsExact_t isExact,
                     unsigned sourceIndex, MetadataPath &&path);

   /// Testify to generic parameters in the Self type of a protocol
   /// witness method.
   void considerWitnessSelf(CanPILFunctionType fnType);

   /// Testify to generic parameters in the Self type of an @objc
   /// generic or protocol method.
   void considerObjCGenericSelf(CanPILFunctionType fnType);

   void considerParameter(PILParameterInfo param, unsigned paramIndex,
                          bool isSelfParameter);

   void addSelfMetadataFulfillment(CanType arg);
   void addSelfWitnessTableFulfillment(CanType arg,
                                       InterfaceConformanceRef conformance);

   void addPseudogenericFulfillments();

   struct FulfillmentMapCallback : FulfillmentMap::InterestingKeysCallback {
      PolymorphicConvention &Self;
      FulfillmentMapCallback(PolymorphicConvention &self) : Self(self) {}

      bool isInterestingType(CanType type) const override {
         return type->isTypeParameter();
      }
      bool hasInterestingType(CanType type) const override {
         return type->hasTypeParameter();
      }
      bool hasLimitedInterestingConformances(CanType type) const override {
         return true;
      }
      GenericSignature::ConformsToArray
      getInterestingConformances(CanType type) const override {
         return Self.getConformsTo(type);
      }
      CanType getSuperclassBound(CanType type) const override {
         return Self.getSuperclassBound(type);
      }
   };
};

} // end anonymous namespace

PolymorphicConvention::PolymorphicConvention(IRGenModule &IGM,
                                             CanPILFunctionType fnType)
   : IGM(IGM), M(*IGM.getPolarphpModule()), FnType(fnType) {
   initGenerics();

   auto rep = fnType->getRepresentation();

   if (fnType->isPseudogeneric()) {
      // Interface witnesses still get Self metadata no matter what. The type
      // parameters of Self are pseudogeneric, though.
      if (rep == PILFunctionTypeRepresentation::WitnessMethod)
         considerWitnessSelf(fnType);

      addPseudogenericFulfillments();
      return;
   }

   if (rep == PILFunctionTypeRepresentation::WitnessMethod) {
      // Interface witnesses always derive all polymorphic parameter information
      // from the Self and Self witness table arguments. We also *cannot* consider
      // other arguments; doing so would potentially make the signature
      // incompatible with other witnesses for the same method.
      considerWitnessSelf(fnType);
   } else if (rep == PILFunctionTypeRepresentation::ObjCMethod) {
      // Objective-C thunks for generic methods also always derive all
      // polymorphic parameter information from the Self argument.
      considerObjCGenericSelf(fnType);
   } else {
      // We don't need to pass anything extra as long as all of the
      // archetypes (and their requirements) are producible from
      // arguments.
      unsigned selfIndex = ~0U;
      auto params = fnType->getParameters();

      // Consider 'self' first.
      if (fnType->hasSelfParam()) {
         selfIndex = params.size() - 1;
         considerParameter(params[selfIndex], selfIndex, true);
      }

      // Now consider the rest of the parameters.
      for (auto index : indices(params)) {
         if (index != selfIndex)
            considerParameter(params[index], index, false);
      }
   }
}

void PolymorphicConvention::addPseudogenericFulfillments() {
   enumerateRequirements([&](GenericRequirement reqt) {
      MetadataPath path;
      path.addImpossibleComponent();

      unsigned sourceIndex = 0; // unimportant, since impossible
      Fulfillments.addFulfillment({reqt.TypeParameter, reqt.Interface},
                                  sourceIndex, std::move(path),
                                  MetadataState::Complete);
   });
}

void
irgen::enumerateGenericSignatureRequirements(CanGenericSignature signature,
                                             const RequirementCallback &callback) {
   if (!signature) return;

   // Get all of the type metadata.
   signature->forEachParam([&](GenericTypeParamType *gp, bool canonical) {
      if (canonical)
         callback({CanType(gp), nullptr});
   });

   // Get the protocol conformances.
   for (auto &reqt : signature->getRequirements()) {
      switch (reqt.getKind()) {
         // Ignore these; they don't introduce extra requirements.
         case RequirementKind::Superclass:
         case RequirementKind::SameType:
         case RequirementKind::Layout:
            continue;

         case RequirementKind::Conformance: {
            auto type = CanType(reqt.getFirstType());
            auto protocol =
               cast<InterfaceType>(CanType(reqt.getSecondType()))->getDecl();
            if (lowering::TypeConverter::interfaceRequiresWitnessTable(protocol)) {
               callback({type, protocol});
            }
            continue;
         }
      }
      llvm_unreachable("bad requirement kind");
   }
}

void
PolymorphicConvention::enumerateRequirements(const RequirementCallback &callback) {
   return enumerateGenericSignatureRequirements(Generics, callback);
}

void PolymorphicConvention::
enumerateUnfulfilledRequirements(const RequirementCallback &callback) {
   enumerateRequirements([&](GenericRequirement requirement) {
      if (requirement.Interface) {
         if (!Fulfillments.getWitnessTable(requirement.TypeParameter,
                                           requirement.Interface)) {
            callback(requirement);
         }
      } else {
         if (!Fulfillments.getTypeMetadata(requirement.TypeParameter)) {
            callback(requirement);
         }
      }
   });
}

void PolymorphicConvention::initGenerics() {
   Generics = FnType->getInvocationGenericSignature();
}

void PolymorphicConvention::considerNewTypeSource(MetadataSource::Kind kind,
                                                  unsigned paramIndex,
                                                  CanType type,
                                                  IsExact_t isExact) {
   if (!Fulfillments.isInterestingTypeForFulfillments(type)) return;

   // Prospectively add a source.
   Sources.emplace_back(kind, paramIndex, type);

   // Consider the source.
   if (!considerType(type, isExact, Sources.size() - 1, MetadataPath())) {
      // If it wasn't used in any fulfillments, remove it.
      Sources.pop_back();
   }
}

bool PolymorphicConvention::considerType(CanType type, IsExact_t isExact,
                                         unsigned sourceIndex,
                                         MetadataPath &&path) {
   FulfillmentMapCallback callbacks(*this);
   return Fulfillments.searchTypeMetadata(IGM, type, isExact,
                                          MetadataState::Complete, sourceIndex,
                                          std::move(path), callbacks);
}

void PolymorphicConvention::considerWitnessSelf(CanPILFunctionType fnType) {
   CanType selfTy = fnType->getSelfInstanceType(IGM.getPILModule());
   auto conformance = fnType->getWitnessMethodConformanceOrInvalid();

   // First, bind type metadata for Self.
   Sources.emplace_back(MetadataSource::Kind::SelfMetadata,
                        MetadataSource::InvalidSourceIndex,
                        selfTy);

   if (selfTy->is<GenericTypeParamType>()) {
      // The Self type is abstract, so we can fulfill its metadata from
      // the Self metadata parameter.
      addSelfMetadataFulfillment(selfTy);
   }

   considerType(selfTy, IsInexact, Sources.size() - 1, MetadataPath());

   // The witness table for the Self : P conformance can be
   // fulfilled from the Self witness table parameter.
   Sources.emplace_back(MetadataSource::Kind::SelfWitnessTable,
                        MetadataSource::InvalidSourceIndex, selfTy);
   addSelfWitnessTableFulfillment(selfTy, conformance);
}

void PolymorphicConvention::considerObjCGenericSelf(CanPILFunctionType fnType) {
   // If this is a static method, get the instance type.
   CanType selfTy = fnType->getSelfInstanceType(IGM.getPILModule());
   unsigned paramIndex = fnType->getParameters().size() - 1;

   // Bind type metadata for Self.
   Sources.emplace_back(MetadataSource::Kind::ClassPointer, paramIndex,
                        selfTy);

   if (isa<GenericTypeParamType>(selfTy))
      addSelfMetadataFulfillment(selfTy);
   else
      considerType(selfTy, IsInexact,
                   Sources.size() - 1, MetadataPath());
}

void PolymorphicConvention::considerParameter(PILParameterInfo param,
                                              unsigned paramIndex,
                                              bool isSelfParameter) {
   auto type = param.getArgumentType(IGM.getPILModule(), FnType);
   switch (param.getConvention()) {
      // Indirect parameters do give us a value we can use, but right now
      // we don't bother, for no good reason. But if this is 'self',
      // consider passing an extra metatype.
      case ParameterConvention::Indirect_In:
      case ParameterConvention::Indirect_In_Constant:
      case ParameterConvention::Indirect_In_Guaranteed:
      case ParameterConvention::Indirect_Inout:
      case ParameterConvention::Indirect_InoutAliasable:
         if (!isSelfParameter) return;
         if (type->getNominalOrBoundGenericNominal()) {
            considerNewTypeSource(MetadataSource::Kind::GenericLValueMetadata,
                                  paramIndex, type, IsExact);
         }
         return;

      case ParameterConvention::Direct_Owned:
      case ParameterConvention::Direct_Unowned:
      case ParameterConvention::Direct_Guaranteed:
         // Classes are sources of metadata.
         if (type->getClassOrBoundGenericClass()) {
            considerNewTypeSource(MetadataSource::Kind::ClassPointer,
                                  paramIndex, type, IsInexact);
            return;
         }

         if (isa<GenericTypeParamType>(type)) {
            if (auto superclassTy = getSuperclassBound(type)) {
               considerNewTypeSource(MetadataSource::Kind::ClassPointer,
                                     paramIndex, superclassTy, IsInexact);
               return;

            }
         }

         // Thick metatypes are sources of metadata.
         if (auto metatypeTy = dyn_cast<MetatypeType>(type)) {
            if (metatypeTy->getRepresentation() != MetatypeRepresentation::Thick)
               return;

            // Thick metatypes for Objective-C parameterized classes are not
            // sources of metadata.
            CanType objTy = metatypeTy.getInstanceType();
            // @todo
//            if (auto classDecl = objTy->getClassOrBoundGenericClass())
//               if (classDecl->usesObjCGenericsModel())
//                  return;

            considerNewTypeSource(MetadataSource::Kind::Metadata,
                                  paramIndex, objTy, IsInexact);
            return;
         }

         return;
   }
   llvm_unreachable("bad parameter convention");
}

void PolymorphicConvention::addSelfMetadataFulfillment(CanType arg) {
   unsigned source = Sources.size() - 1;
   Fulfillments.addFulfillment({arg, nullptr},
                               source, MetadataPath(), MetadataState::Complete);
}

void PolymorphicConvention::addSelfWitnessTableFulfillment(
   CanType arg, InterfaceConformanceRef conformance) {
   auto proto = conformance.getRequirement();
   unsigned source = Sources.size() - 1;
   Fulfillments.addFulfillment({arg, proto},
                               source, MetadataPath(), MetadataState::Complete);

   if (conformance.isConcrete()) {
      FulfillmentMapCallback callbacks(*this);
      Fulfillments.searchConformance(IGM, conformance.getConcrete(), source,
                                     MetadataPath(), callbacks);
   }
}

const Fulfillment *
PolymorphicConvention::getFulfillmentForTypeMetadata(CanType type) const {
   return Fulfillments.getTypeMetadata(type);
}

void irgen::enumerateGenericParamFulfillments(IRGenModule &IGM,
                                              CanPILFunctionType fnType,
                                              GenericParamFulfillmentCallback callback) {
   PolymorphicConvention convention(IGM, fnType);

   // Check if any requirements were fulfilled by metadata stored inside a
   // captured value.
   auto generics = fnType->getInvocationGenericSignature();

   for (auto genericParam : generics->getGenericParams()) {
      auto genericParamType = genericParam->getCanonicalType();

      auto fulfillment
         = convention.getFulfillmentForTypeMetadata(genericParamType);
      if (fulfillment == nullptr)
         continue;

      auto &source = convention.getSource(fulfillment->SourceIndex);
      callback(genericParamType, source, fulfillment->Path);
   }
}

namespace {

/// A class for binding type parameters of a generic function.
class EmitPolymorphicParameters : public PolymorphicConvention {
   IRGenFunction &IGF;
   PILFunction &Fn;

public:
   EmitPolymorphicParameters(IRGenFunction &IGF, PILFunction &Fn);

   void emit(Explosion &in, WitnessMetadata *witnessMetadata,
             const GetParameterFn &getParameter);

private:
   CanType getTypeInContext(CanType type) const;

   CanType getArgTypeInContext(unsigned paramIndex) const;

   /// Fulfill local type data from any extra information associated with
   /// the given source.
   void bindExtraSource(const MetadataSource &source, Explosion &in,
                        WitnessMetadata *witnessMetadata);

   void bindParameterSources(const GetParameterFn &getParameter);

   void bindParameterSource(PILParameterInfo param, unsigned paramIndex,
                            const GetParameterFn &getParameter) ;
   // Did the convention decide that the parameter at the given index
   // was a class-pointer source?
   bool isClassPointerSource(unsigned paramIndex);
};

} // end anonymous namespace

EmitPolymorphicParameters::EmitPolymorphicParameters(IRGenFunction &IGF,
                                                     PILFunction &Fn)
   : PolymorphicConvention(IGF.IGM, Fn.getLoweredFunctionType()),
     IGF(IGF), Fn(Fn) {}


CanType EmitPolymorphicParameters::getTypeInContext(CanType type) const {
   return Fn.mapTypeIntoContext(type)->getCanonicalType();
}

CanType EmitPolymorphicParameters::getArgTypeInContext(unsigned paramIndex) const {
   return getTypeInContext(FnType->getParameters()[paramIndex]
                              .getArgumentType(IGM.getPILModule(), FnType));
}

void EmitPolymorphicParameters::bindExtraSource(const MetadataSource &source,
                                                Explosion &in,
                                                WitnessMetadata *witnessMetadata) {
   switch (source.getKind()) {
      case MetadataSource::Kind::Metadata:
      case MetadataSource::Kind::ClassPointer:
         // Ignore these, we'll get to them when we walk the parameter list.
         return;

      case MetadataSource::Kind::GenericLValueMetadata: {
         CanType argTy = getArgTypeInContext(source.getParamIndex());

         llvm::Value *metadata = in.claimNext();
         setTypeMetadataName(IGF.IGM, metadata, argTy);

         IGF.bindLocalTypeDataFromTypeMetadata(argTy, IsExact, metadata,
                                               MetadataState::Complete);
         return;
      }

      case MetadataSource::Kind::SelfMetadata: {
         assert(witnessMetadata && "no metadata for witness method");
         llvm::Value *metadata = witnessMetadata->SelfMetadata;
         assert(metadata && "no Self metadata for witness method");

         // Mark this as the cached metatype for Self.
         auto selfTy = FnType->getSelfInstanceType(IGM.getPILModule());
         CanType argTy = getTypeInContext(selfTy);
         setTypeMetadataName(IGF.IGM, metadata, argTy);
         auto *CD = selfTy.getClassOrBoundGenericClass();
         // The self metadata here corresponds to the conforming type.
         // For an inheritable conformance, that may be a subclass of the static
         // type, and so the self metadata will be inexact. Currently, all
         // conformances are inheritable.
         IGF.bindLocalTypeDataFromTypeMetadata(
            argTy, (!CD || CD->isFinal()) ? IsExact : IsInexact, metadata,
            MetadataState::Complete);
         return;
      }

      case MetadataSource::Kind::SelfWitnessTable: {
         assert(witnessMetadata && "no metadata for witness method");
         llvm::Value *selfTable = witnessMetadata->SelfWitnessTable;
         assert(selfTable && "no Self witness table for witness method");

         // Mark this as the cached witness table for Self.
         auto conformance = FnType->getWitnessMethodConformanceOrInvalid();
         auto selfProto = conformance.getRequirement();

         auto selfTy = FnType->getSelfInstanceType(IGM.getPILModule());
         CanType argTy = getTypeInContext(selfTy);

         setInterfaceWitnessTableName(IGF.IGM, selfTable, argTy, selfProto);
         IGF.setUnscopedLocalTypeData(
            argTy,
            LocalTypeDataKind::forInterfaceWitnessTable(conformance),
            selfTable);

         if (conformance.isConcrete()) {
            IGF.bindLocalTypeDataFromSelfWitnessTable(
               conformance.getConcrete(),
               selfTable,
               [this](CanType type) {
                  return getTypeInContext(type);
               });
         }
         return;
      }
   }
   llvm_unreachable("bad source kind!");
}

void EmitPolymorphicParameters::bindParameterSources(const GetParameterFn &getParameter) {
   auto params = FnType->getParameters();

   // Bind things from 'self' preferentially.
   if (FnType->hasSelfParam()) {
      bindParameterSource(params.back(), params.size() - 1, getParameter);
      params = params.drop_back();
   }

   for (unsigned index : indices(params)) {
      bindParameterSource(params[index], index, getParameter);
   }
}

void EmitPolymorphicParameters::
bindParameterSource(PILParameterInfo param, unsigned paramIndex,
                    const GetParameterFn &getParameter) {
   // Ignore indirect parameters for now.  This is potentially dumb.
   if (IGF.IGM.silConv.isPILIndirect(param))
      return;

   CanType paramType = getArgTypeInContext(paramIndex);

   // If the parameter is a thick metatype, bind it directly.
   // TODO: objc metatypes?
   if (auto metatype = dyn_cast<MetatypeType>(paramType)) {
      if (metatype->getRepresentation() == MetatypeRepresentation::Thick) {
         paramType = metatype.getInstanceType();
         llvm::Value *metadata = getParameter(paramIndex);
         IGF.bindLocalTypeDataFromTypeMetadata(paramType, IsInexact, metadata,
                                               MetadataState::Complete);
      } else if (metatype->getRepresentation() == MetatypeRepresentation::ObjC) {
         paramType = metatype.getInstanceType();
         llvm::Value *objcMetatype = getParameter(paramIndex);
         auto *metadata = emitObjCMetadataRefForMetadata(IGF, objcMetatype);
         IGF.bindLocalTypeDataFromTypeMetadata(paramType, IsInexact, metadata,
                                               MetadataState::Complete);
      }
      return;
   }

   // If the parameter is a class type, we only consider it interesting
   // if the convention decided it was actually a source.
   // TODO: if the class pointer is guaranteed, we can do this lazily,
   // at which point it might make sense to do it for a wider selection
   // of types.
   if (isClassPointerSource(paramIndex)) {
      llvm::Value *instanceRef = getParameter(paramIndex);
      PILType instanceType = PILType::getPrimitiveObjectType(paramType);
      llvm::Value *metadata =
         emitDynamicTypeOfHeapObject(IGF, instanceRef,
                                     MetatypeRepresentation::Thick,
                                     instanceType,
            /*allow artificial subclasses*/ true);
      IGF.bindLocalTypeDataFromTypeMetadata(paramType, IsInexact, metadata,
                                            MetadataState::Complete);
      return;
   }
}

bool EmitPolymorphicParameters::isClassPointerSource(unsigned paramIndex) {
   for (auto &source : getSources()) {
      if (source.getKind() == MetadataSource::Kind::ClassPointer &&
          source.getParamIndex() == paramIndex) {
         return true;
      }
   }
   return false;
}

namespace {

/// A class for binding type parameters of a generic function.
class BindPolymorphicParameter : public PolymorphicConvention {
   IRGenFunction &IGF;
   CanPILFunctionType &SubstFnType;

public:
   BindPolymorphicParameter(IRGenFunction &IGF, CanPILFunctionType &origFnType,
                            CanPILFunctionType &SubstFnType)
      : PolymorphicConvention(IGF.IGM, origFnType), IGF(IGF),
        SubstFnType(SubstFnType) {}

   void emit(Explosion &in, unsigned paramIndex);

private:
   // Did the convention decide that the parameter at the given index
   // was a class-pointer source?
   bool isClassPointerSource(unsigned paramIndex);
};

} // end anonymous namespace

bool BindPolymorphicParameter::isClassPointerSource(unsigned paramIndex) {
   for (auto &source : getSources()) {
      if (source.getKind() == MetadataSource::Kind::ClassPointer &&
          source.getParamIndex() == paramIndex) {
         return true;
      }
   }
   return false;
}

void BindPolymorphicParameter::emit(Explosion &nativeParam, unsigned paramIndex) {
   if (!isClassPointerSource(paramIndex))
      return;

   assert(nativeParam.size() == 1);
   auto paramType = SubstFnType->getParameters()[paramIndex]
      .getArgumentType(IGM.getPILModule(), SubstFnType);
   llvm::Value *instanceRef = nativeParam.getAll()[0];
   PILType instanceType = PILType::getPrimitiveObjectType(paramType);
   llvm::Value *metadata =
      emitDynamicTypeOfHeapObject(IGF, instanceRef,
                                  MetatypeRepresentation::Thick,
                                  instanceType,
         /* allow artificial subclasses */ true);
   IGF.bindLocalTypeDataFromTypeMetadata(paramType, IsInexact, metadata,
                                         MetadataState::Complete);
}

void irgen::bindPolymorphicParameter(IRGenFunction &IGF,
                                     CanPILFunctionType &OrigFnType,
                                     CanPILFunctionType &SubstFnType,
                                     Explosion &nativeParam,
                                     unsigned paramIndex) {
   BindPolymorphicParameter(IGF, OrigFnType, SubstFnType)
      .emit(nativeParam, paramIndex);
}

static bool shouldSetName(IRGenModule &IGM, llvm::Value *value, CanType type) {
   // If value names are globally disabled, honor that.
   if (!IGM.EnableValueNames) return false;

   // Suppress value names for values with opened existentials.
   if (type->hasOpenedExistential()) return false;

   // If the value already has a name, honor that.
   if (value->hasName()) return false;

   // Only do this for local values.
   return (isa<llvm::Instruction>(value) || isa<llvm::Argument>(value));
}

void irgen::setTypeMetadataName(IRGenModule &IGM, llvm::Value *metadata,
                                CanType type) {
   if (!shouldSetName(IGM, metadata, type)) return;

   SmallString<128> name; {
      llvm::raw_svector_ostream out(name);
      type.print(out);
   }
   metadata->setName(type->getString());
}

void irgen::setInterfaceWitnessTableName(IRGenModule &IGM, llvm::Value *wtable,
                                         CanType type,
                                         InterfaceDecl *requirement) {
   if (!shouldSetName(IGM, wtable, type)) return;

   SmallString<128> name; {
      llvm::raw_svector_ostream out(name);
      type.print(out);
      out << '.' << requirement->getNameStr();
   }
   wtable->setName(name);
}

namespace {
/// A class which lays out a witness table in the abstract.
class WitnessTableLayout : public PILWitnessVisitor<WitnessTableLayout> {
   SmallVector<WitnessTableEntry, 16> Entries;
   bool requirementSignatureOnly;

public:
   explicit WitnessTableLayout(InterfaceInfoKind resultKind) {
      switch (resultKind) {
         case InterfaceInfoKind::RequirementSignature:
            requirementSignatureOnly = true;
            break;
         case InterfaceInfoKind::Full:
            requirementSignatureOnly = false;
            break;
      }
   }

   bool shouldVisitRequirementSignatureOnly() {
      return requirementSignatureOnly;
   }

   void addInterfaceConformanceDescriptor() { }

   /// The next witness is an out-of-line base protocol.
   void addOutOfLineBaseInterface(InterfaceDecl *baseProto) {
      Entries.push_back(WitnessTableEntry::forOutOfLineBase(baseProto));
   }

   void addMethod(PILDeclRef func) {
      auto decl = cast<AbstractFunctionDecl>(func.getDecl());
      Entries.push_back(WitnessTableEntry::forFunction(decl));
   }

   void addPlaceholder(MissingMemberDecl *placeholder) {
      for (auto i : range(placeholder->getNumberOfVTableEntries())) {
         (void)i;
         Entries.push_back(WitnessTableEntry());
      }
   }

   void addAssociatedType(AssociatedType requirement) {
      Entries.push_back(WitnessTableEntry::forAssociatedType(requirement));
   }

   void addAssociatedConformance(const AssociatedConformance &req) {
      Entries.push_back(WitnessTableEntry::forAssociatedConformance(req));
   }

   ArrayRef<WitnessTableEntry> getEntries() const { return Entries; }
};
} // end anonymous namespace

/// Return true if the witness table requires runtime instantiation to
/// handle resiliently-added requirements with default implementations.
bool IRGenModule::isResilientConformance(
   const NormalInterfaceConformance *conformance) {
   // If the protocol is not resilient, the conformance is not resilient
   // either.
   if (!conformance->getInterface()->isResilient())
      return false;

   auto *conformanceModule = conformance->getDeclContext()->getParentModule();

   // If the protocol and the conformance are both in the current module,
   // they're not resilient.
   if (conformanceModule == getPolarphpModule() &&
       conformanceModule == conformance->getInterface()->getParentModule())
      return false;

   // If the protocol and the conformance are in the same module and the
   // conforming type is not generic, they're not resilient.
   //
   // This is an optimization -- a conformance of a non-generic type cannot
   // resiliently become dependent.
   if (!conformance->getDeclContext()->isGenericContext() &&
       conformanceModule == conformance->getInterface()->getParentModule())
      return false;

   // We have a resilient conformance.
   return true;
}

bool IRGenModule::isResilientConformance(const RootInterfaceConformance *root) {
   if (auto normal = dyn_cast<NormalInterfaceConformance>(root))
      return isResilientConformance(normal);
   // Self-conformances never require this.
   return false;
}

/// Whether this protocol conformance has a dependent type witness.
static bool hasDependentTypeWitness(
   const NormalInterfaceConformance *conformance) {
   auto DC = conformance->getDeclContext();
   // If the conforming type isn't dependent, the below check is never true.
   if (!DC->isGenericContext())
      return false;

   // Check whether any of the associated types are dependent.
   if (conformance->forEachTypeWitness(
      [&](AssociatedTypeDecl *requirement, Type type,
          TypeDecl *explicitDecl) -> bool {
         // Skip associated types that don't have witness table entries.
         if (!requirement->getOverriddenDecls().empty())
            return false;

         // REPILIENCE: this could be an opaque conformance
         return type->getCanonicalType()->hasTypeParameter();
      },
      /*useResolver=*/true)) {
      return true;
   }

   return false;
}

static bool isDependentConformance(
   IRGenModule &IGM,
   const RootInterfaceConformance *rootConformance,
   llvm::SmallPtrSet<const NormalInterfaceConformance *, 4> &visited){
   // Self-conformances are never dependent.
   auto conformance = dyn_cast<NormalInterfaceConformance>(rootConformance);
   if (!conformance)
      return false;

   // Check whether we've visited this conformance already.  If so,
   // optimistically assume it's fine --- we want the maximal fixed point.
   if (!visited.insert(conformance).second)
      return false;

   // If the conformance is resilient, this is always true.
   if (IGM.isResilientConformance(conformance))
      return true;

   // Check whether any of the conformances are dependent.
   auto proto = conformance->getInterface();
   for (const auto &req : proto->getRequirementSignature()) {
      if (req.getKind() != RequirementKind::Conformance)
         continue;

      auto assocInterface = req.getSecondType()->castTo<InterfaceType>()->getDecl();
      // @todo
//      if (assocInterface->isObjC())
//         continue;

      auto assocConformance =
         conformance->getAssociatedConformance(req.getFirstType(), assocInterface);
      if (assocConformance.isAbstract() ||
          isDependentConformance(IGM,
                                 assocConformance.getConcrete()
                                    ->getRootConformance(),
                                 visited))
         return true;
   }

   if (hasDependentTypeWitness(conformance))
      return true;

   // Check if there are any conditional conformances. Other forms of conditional
   // requirements don't exist in the witness table.
   return PILWitnessTable::enumerateWitnessTableConditionalConformances(
      conformance, [](unsigned, CanType, InterfaceDecl *) { return true; });
}

/// Is there anything about the given conformance that requires witness
/// tables to be dependently-generated?
bool IRGenModule::isDependentConformance(
   const RootInterfaceConformance *conformance) {
   llvm::SmallPtrSet<const NormalInterfaceConformance *, 4> visited;
   return ::isDependentConformance(*this, conformance, visited);
}

static bool isSynthesizedNonUnique(const RootInterfaceConformance *conformance) {
   if (auto normal = dyn_cast<NormalInterfaceConformance>(conformance))
      return normal->isSynthesizedNonUnique();
   return false;
}

static llvm::Value *
emitConditionalConformancesBuffer(IRGenFunction &IGF,
                                  const InterfaceConformance *substConformance) {
   auto rootConformance =
      dyn_cast<NormalInterfaceConformance>(substConformance->getRootConformance());

   // Not a normal conformance means no conditional requirements means no need
   // for a buffer.
   if (!rootConformance)
      return llvm::UndefValue::get(IGF.IGM.WitnessTablePtrPtrTy);

   // Pointers to the witness tables, in the right order, which will be included
   // in the buffer that gets passed to the witness table accessor.
   llvm::SmallVector<llvm::Value *, 4> tables;

   auto subMap = substConformance->getSubstitutions(IGF.IGM.getPolarphpModule());

   PILWitnessTable::enumerateWitnessTableConditionalConformances(
      rootConformance, [&](unsigned, CanType type, InterfaceDecl *proto) {
         auto substType = type.subst(subMap)->getCanonicalType();
         auto reqConformance = subMap.lookupConformance(type, proto);
         assert(reqConformance && "conditional conformance must be valid");

         tables.push_back(emitWitnessTableRef(IGF, substType, reqConformance));
         return /*finished?*/ false;
      });

   // No conditional requirements means no need for a buffer.
   if (tables.empty()) {
      return llvm::UndefValue::get(IGF.IGM.WitnessTablePtrPtrTy);
   }

   auto buffer = IGF.createAlloca(
      llvm::ArrayType::get(IGF.IGM.WitnessTablePtrTy, tables.size()),
      IGF.IGM.getPointerAlignment(), "conditional.requirement.buffer");
   buffer = IGF.Builder.CreateStructGEP(buffer, 0, Size(0));

   // Write each of the conditional witness tables into the buffer.
   for (auto idx : indices(tables)) {
      auto slot =
         IGF.Builder.CreateConstArrayGEP(buffer, idx, IGF.IGM.getPointerSize());
      IGF.Builder.CreateStore(tables[idx], slot);
   }

   return buffer.getAddress();
}

static llvm::Value *emitWitnessTableAccessorCall(
   IRGenFunction &IGF, const InterfaceConformance *conformance,
   llvm::Value **srcMetadataCache) {
   auto conformanceDescriptor =
      IGF.IGM.getAddrOfInterfaceConformanceDescriptor(
         conformance->getRootConformance());

   // Emit the source metadata if we haven't yet.
   if (!*srcMetadataCache) {
      *srcMetadataCache = IGF.emitAbstractTypeMetadataRef(
         conformance->getType()->getCanonicalType());
   }

   auto conditionalTables =
      emitConditionalConformancesBuffer(IGF, conformance);

   auto call = IGF.Builder.CreateCall(IGF.IGM.getGetWitnessTableFn(),
                                      {conformanceDescriptor, *srcMetadataCache,
                                       conditionalTables});

   call->setCallingConv(IGF.IGM.DefaultCC);
   call->setDoesNotThrow();

   return call;
}

/// Fetch the lazy access function for the given conformance of the
/// given type.
static llvm::Function *
getWitnessTableLazyAccessFunction(IRGenModule &IGM,
                                  const InterfaceConformance *conformance) {
   auto conformingType = conformance->getType()->getCanonicalType();
   assert(!conformingType->hasArchetype());

   auto rootConformance = conformance->getRootNormalConformance();
   llvm::Function *accessor = IGM.getAddrOfWitnessTableLazyAccessFunction(
      rootConformance, conformingType, ForDefinition);

   // If we're not supposed to define the accessor, or if we already
   // have defined it, just return the pointer.
   if (!accessor->empty())
      return accessor;

   if (IGM.getOptions().optimizeForSize())
      accessor->addFnAttr(llvm::Attribute::NoInline);

   // Okay, define the accessor.
   auto cacheVariable =
      cast<llvm::GlobalVariable>(IGM.getAddrOfWitnessTableLazyCacheVariable(
         rootConformance, conformingType, ForDefinition));
   emitCacheAccessFunction(IGM, accessor, cacheVariable, CacheStrategy::Lazy,
                           [&](IRGenFunction &IGF, Explosion &params) {
                              llvm::Value *conformingMetadataCache = nullptr;
                              return MetadataResponse::forComplete(
                                 emitWitnessTableAccessorCall(IGF, conformance,
                                                              &conformingMetadataCache));
                           });

   return accessor;
}

static const InterfaceConformance &
mapConformanceIntoContext(IRGenModule &IGM, const RootInterfaceConformance &conf,
                          DeclContext *dc) {
   auto normal = dyn_cast<NormalInterfaceConformance>(&conf);
   if (!normal) return conf;
   return *conf.subst([&](SubstitutableType *t) -> Type {
                         return dc->mapTypeIntoContext(t);
                      },
                      LookUpConformanceInModule(IGM.getPolarphpModule()));
}

WitnessIndex InterfaceInfo::getAssociatedTypeIndex(
   IRGenModule &IGM,
   AssociatedType assocType) const {
   assert(!IGM.isResilient(assocType.getSourceInterface(),
                           ResilienceExpansion::Maximal) &&
          "Cannot ask for the associated type index of non-resilient protocol");
   for (auto &witness : getWitnessEntries()) {
      if (witness.matchesAssociatedType(assocType))
         return getNonBaseWitnessIndex(&witness);
   }
   llvm_unreachable("didn't find entry for associated type");
}

namespace {

/// Conformance info for a witness table that can be directly generated.
class DirectConformanceInfo : public ConformanceInfo {
   friend InterfaceInfo;

   const RootInterfaceConformance *RootConformance;
public:
   DirectConformanceInfo(const RootInterfaceConformance *C)
      : RootConformance(C) {}

   llvm::Value *getTable(IRGenFunction &IGF,
                         llvm::Value **conformingMetadataCache) const override {
      return IGF.IGM.getAddrOfWitnessTable(RootConformance);
   }

   llvm::Constant *tryGetConstantTable(IRGenModule &IGM,
                                       CanType conformingType) const override {
      if (IGM.getOptions().LazyInitializeInterfaceConformances &&
          RootConformance->getDeclContext()->getParentModule() != IGM.getPolarphpModule())
         return nullptr;
      return IGM.getAddrOfWitnessTable(RootConformance);
   }
};

/// Conformance info for a witness table that is (or may be) dependent.
class AccessorConformanceInfo : public ConformanceInfo {
   friend InterfaceInfo;

   const InterfaceConformance *Conformance;

public:
   AccessorConformanceInfo(const InterfaceConformance *C) : Conformance(C) {}

   llvm::Value *getTable(IRGenFunction &IGF,
                         llvm::Value **typeMetadataCache) const override {
      // If we're looking up a dependent type, we can't cache the result.
      if (Conformance->getType()->hasArchetype() ||
          Conformance->getType()->hasDynamicSelfType()) {
         return emitWitnessTableAccessorCall(IGF, Conformance,
                                             typeMetadataCache);
      }

      // Otherwise, call a lazy-cache function.
      auto accessor =
         getWitnessTableLazyAccessFunction(IGF.IGM, Conformance);
      llvm::CallInst *call = IGF.Builder.CreateCall(accessor, {});
      call->setCallingConv(IGF.IGM.DefaultCC);
      call->setDoesNotAccessMemory();
      call->setDoesNotThrow();

      return call;
   }

   llvm::Constant *tryGetConstantTable(IRGenModule &IGM,
                                       CanType conformingType) const override {
      return nullptr;
   }
};

/// A class which lays out a specific conformance to a protocol.
class WitnessTableBuilder : public PILWitnessVisitor<WitnessTableBuilder> {
   IRGenModule &IGM;
   ConstantArrayBuilder &Table;
   unsigned TableSize = ~0U; // will get overwritten unconditionally
   PILWitnessTable *PILWT;
   CanType ConcreteType;
   const RootInterfaceConformance &Conformance;
   const InterfaceConformance &ConformanceInContext;
   ArrayRef<PILWitnessTable::Entry> PILEntries;
   ArrayRef<PILWitnessTable::ConditionalConformance>
      PILConditionalConformances;

   Optional<FulfillmentMap> Fulfillments;
   SmallVector<std::pair<size_t, const ConformanceInfo *>, 4>
      SpecializedBaseConformances;

   SmallVector<size_t, 4> ConditionalRequirementPrivateDataIndices;

   // Conditional conformances and metadata caches are stored at negative
   // offsets, with conditional conformances closest to 0.
   unsigned NextPrivateDataIndex = 0;
   bool ResilientConformance;

   const InterfaceInfo &PI;

public:
   WitnessTableBuilder(IRGenModule &IGM, ConstantArrayBuilder &table,
                       PILWitnessTable *PILWT)
      : IGM(IGM), Table(table), PILWT(PILWT),
        ConcreteType(PILWT->getConformance()->getDeclContext()
                        ->mapTypeIntoContext(
                           PILWT->getConformance()->getType())
                        ->getCanonicalType()),
        Conformance(*PILWT->getConformance()),
        ConformanceInContext(
           mapConformanceIntoContext(IGM, Conformance,
                                     Conformance.getDeclContext())),
        PILEntries(PILWT->getEntries()),
        PILConditionalConformances(PILWT->getConditionalConformances()),
        ResilientConformance(IGM.isResilientConformance(&Conformance)),
        PI(IGM.getInterfaceInfo(PILWT->getConformance()->getInterface(),
                                (ResilientConformance
                                 ? InterfaceInfoKind::RequirementSignature
                                 : InterfaceInfoKind::Full))) {}

   /// The number of entries in the witness table.
   unsigned getTableSize() const { return TableSize; }

   /// The number of private entries in the witness table.
   unsigned getTablePrivateSize() const { return NextPrivateDataIndex; }

   /// The top-level entry point.
   void build();

   /// Add reference to the protocol conformance descriptor that generated
   /// this table.
   void addInterfaceConformanceDescriptor() {
      auto descriptor =
         IGM.getAddrOfInterfaceConformanceDescriptor(&Conformance);
      Table.addBitCast(descriptor, IGM.Int8PtrTy);
   }

   /// A base protocol is witnessed by a pointer to the conformance
   /// of this type to that protocol.
   void addOutOfLineBaseInterface(InterfaceDecl *baseProto) {
#ifndef NDEBUG
      auto &entry = PILEntries.front();
#endif
      PILEntries = PILEntries.slice(1);

      // Resilient conformances get a resilient witness table.
      if (ResilientConformance)
         return;

#ifndef NDEBUG
      assert(entry.getKind() == PILWitnessTable::BaseInterface
             && "sil witness table does not match protocol");
      assert(entry.getBaseInterfaceWitness().Requirement == baseProto
             && "sil witness table does not match protocol");
      auto piIndex = PI.getBaseIndex(baseProto);
      assert((size_t)piIndex.getValue() ==
             Table.size() - WitnessTableFirstRequirementOffset &&
             "offset doesn't match InterfaceInfo layout");
#endif

      // TODO: Use the witness entry instead of falling through here.

      // Look for conformance info.
      auto *astConf = ConformanceInContext.getInheritedConformance(baseProto);
      assert(astConf->getType()->isEqual(ConcreteType));
      const ConformanceInfo &conf = IGM.getConformanceInfo(baseProto, astConf);

      // If we can emit the base witness table as a constant, do so.
      llvm::Constant *baseWitness = conf.tryGetConstantTable(IGM, ConcreteType);
      if (baseWitness) {
         Table.addBitCast(baseWitness, IGM.Int8PtrTy);
         return;
      }

      // Otherwise, we'll need to derive it at instantiation time.
      SpecializedBaseConformances.push_back({Table.size(), &conf});
      Table.addNullPointer(IGM.Int8PtrTy);
   }

   void addMethod(PILDeclRef requirement) {
      auto &entry = PILEntries.front();
      PILEntries = PILEntries.slice(1);

      // Resilient conformances get a resilient witness table.
      if (ResilientConformance)
         return;

#ifndef NDEBUG
      assert(entry.getKind() == PILWitnessTable::Method
             && "sil witness table does not match protocol");
      assert(entry.getMethodWitness().Requirement == requirement
             && "sil witness table does not match protocol");
      auto piIndex =
         PI.getFunctionIndex(cast<AbstractFunctionDecl>(requirement.getDecl()));
      assert((size_t)piIndex.getValue() ==
             Table.size() - WitnessTableFirstRequirementOffset &&
             "offset doesn't match InterfaceInfo layout");
#endif

      PILFunction *Func = entry.getMethodWitness().Witness;
      llvm::Constant *witness = nullptr;
      if (Func) {
         witness = IGM.getAddrOfPILFunction(Func, NotForDefinition);
      } else {
         // The method is removed by dead method elimination.
         // It should be never called. We add a pointer to an error function.
         witness = IGM.getDeletedMethodErrorFn();
      }
      Table.addBitCast(witness, IGM.Int8PtrTy);
      return;
   }

   void addPlaceholder(MissingMemberDecl *placeholder) {
      llvm_unreachable("cannot emit a witness table with placeholders in it");
   }

   void addAssociatedType(AssociatedType requirement) {
      auto &entry = PILEntries.front();
      PILEntries = PILEntries.slice(1);

      // Resilient conformances get a resilient witness table.
      if (ResilientConformance)
         return;

#ifndef NDEBUG
      assert(entry.getKind() == PILWitnessTable::AssociatedType
             && "sil witness table does not match protocol");
      assert(entry.getAssociatedTypeWitness().Requirement
             == requirement.getAssociation()
             && "sil witness table does not match protocol");
      auto piIndex = PI.getAssociatedTypeIndex(IGM, requirement);
      assert((size_t)piIndex.getValue() ==
             Table.size() - WitnessTableFirstRequirementOffset &&
             "offset doesn't match InterfaceInfo layout");
#else
      (void)entry;
#endif

      auto associate =
         Conformance.getTypeWitness(requirement.getAssociation());
      llvm::Constant *witness =
         IGM.getAssociatedTypeWitness(associate, /*inInterfaceContext=*/false);
      Table.addBitCast(witness, IGM.Int8PtrTy);
   }

   void addAssociatedConformance(AssociatedConformance requirement) {
      // FIXME: Add static witness tables for type conformances.

      auto &entry = PILEntries.front();
      (void)entry;
      PILEntries = PILEntries.slice(1);

      if (ResilientConformance)
         return;

      auto associate =
         ConformanceInContext.getAssociatedType(
            requirement.getAssociation())->getCanonicalType();

      InterfaceConformanceRef associatedConformance =
         ConformanceInContext.getAssociatedConformance(
            requirement.getAssociation(),
            requirement.getAssociatedRequirement());

#ifndef NDEBUG
      assert(entry.getKind() == PILWitnessTable::AssociatedTypeInterface
             && "sil witness table does not match protocol");
      auto associatedWitness = entry.getAssociatedTypeInterfaceWitness();
      assert(associatedWitness.Requirement == requirement.getAssociation()
             && "sil witness table does not match protocol");
      assert(associatedWitness.Interface ==
             requirement.getAssociatedRequirement()
             && "sil witness table does not match protocol");
      auto piIndex = PI.getAssociatedConformanceIndex(requirement);
      assert((size_t)piIndex.getValue() ==
             Table.size() - WitnessTableFirstRequirementOffset &&
             "offset doesn't match InterfaceInfo layout");
#endif

      llvm::Constant *witnessEntry =
         getAssociatedConformanceWitness(requirement, associate,
                                         associatedConformance);
      Table.addBitCast(witnessEntry, IGM.Int8PtrTy);
   }

   /// Build the instantiation function that runs at the end of witness
   /// table specialization.
   llvm::Constant *buildInstantiationFunction();

private:
   void addConditionalConformances() {
      assert(NextPrivateDataIndex == 0);
      for (auto conditional : PILConditionalConformances) {
         // We don't actually need to know anything about the specific
         // conformances here, just make sure we get right private data slots.
         (void)conditional;

         auto reqtIndex = getNextPrivateDataIndex();
         ConditionalRequirementPrivateDataIndices.push_back(reqtIndex);
      }
   }

   void defineAssociatedTypeWitnessTableAccessFunction(
      AssociatedConformance requirement,
      CanType associatedType,
      InterfaceConformanceRef conformance);

   llvm::Constant *getAssociatedConformanceWitness(
      AssociatedConformance requirement,
      CanType associatedType,
      InterfaceConformanceRef conformance);

   /// Allocate another word of private data storage in the conformance table.
   unsigned getNextPrivateDataIndex() {
      return NextPrivateDataIndex++;
   }

   Address getAddressOfPrivateDataSlot(IRGenFunction &IGF, Address table,
                                       unsigned index) {
      assert(index < NextPrivateDataIndex);
      return IGF.Builder.CreateConstArrayGEP(
         table, privateWitnessTableIndexToTableOffset(index),
         IGF.IGM.getPointerSize());
   }

   const FulfillmentMap &getFulfillmentMap() {
      if (Fulfillments) return *Fulfillments;

      Fulfillments.emplace();
      if (ConcreteType->hasArchetype()) {
         struct Callback : FulfillmentMap::InterestingKeysCallback {
            bool isInterestingType(CanType type) const override {
               return isa<ArchetypeType>(type);
            }
            bool hasInterestingType(CanType type) const override {
               return type->hasArchetype();
            }
            bool hasLimitedInterestingConformances(CanType type) const override {
               return false;
            }
            GenericSignature::ConformsToArray
            getInterestingConformances(CanType type) const override {
               llvm_unreachable("no limits");
            }
            CanType getSuperclassBound(CanType type) const override {
               if (auto superclassTy = cast<ArchetypeType>(type)->getSuperclass())
                  return superclassTy->getCanonicalType();
               return CanType();
            }
         } callback;
         Fulfillments->searchTypeMetadata(IGM, ConcreteType, IsExact,
                                          MetadataState::Abstract,
            /*sourceIndex*/ 0, MetadataPath(),
                                          callback);
      }
      return *Fulfillments;
   }

public:
   /// Collect the set of resilient witnesses, which will become part of the
   /// protocol conformance descriptor.
   void collectResilientWitnesses(
      SmallVectorImpl<llvm::Constant *> &resilientWitnesses);
};
} // end anonymous namespace

/// Build the witness table.
void WitnessTableBuilder::build() {
   addConditionalConformances();
   visitInterfaceDecl(Conformance.getInterface());
   TableSize = Table.size();
}

llvm::Constant *IRGenModule::getAssociatedTypeWitness(Type type,
                                                      bool inInterfaceContext) {
   // FIXME: If we can directly reference constant type metadata, do so.

   // Form a reference to the mangled name for this type.
   assert(!type->hasArchetype() && "type cannot contain archetypes");
   auto role = inInterfaceContext
               ? MangledTypeRefRole::DefaultAssociatedTypeWitness
               : MangledTypeRefRole::Metadata;
   auto typeRef = getTypeRef(type, /*generic signature*/nullptr, role).first;

   // Set the low bit to indicate that this is a mangled name.
   auto witness = llvm::ConstantExpr::getPtrToInt(typeRef, IntPtrTy);
   unsigned bit = InterfaceRequirementFlags::AssociatedTypeMangledNameBit;
   auto bitConstant = llvm::ConstantInt::get(IntPtrTy, bit);
   witness = llvm::ConstantExpr::getAdd(witness, bitConstant);
   return llvm::ConstantExpr::getIntToPtr(witness, Int8PtrTy);
}

static void buildAssociatedTypeValueName(CanType depAssociatedType,
                                         SmallString<128> &name) {
   if (auto memberType = dyn_cast<DependentMemberType>(depAssociatedType)) {
      buildAssociatedTypeValueName(memberType.getBase(), name);
      name += '.';
      name += memberType->getName().str();
   } else {
      assert(isa<GenericTypeParamType>(depAssociatedType)); // Self
   }
}

llvm::Constant *WitnessTableBuilder::getAssociatedConformanceWitness(
   AssociatedConformance requirement,
   CanType associatedType,
   InterfaceConformanceRef conformance) {
   defineAssociatedTypeWitnessTableAccessFunction(requirement, associatedType,
                                                  conformance);
   assert(isa<NormalInterfaceConformance>(Conformance) && "has associated type");
   auto conf = cast<NormalInterfaceConformance>(&Conformance);
   return IGM.getMangledAssociatedConformance(conf, requirement);
}

void WitnessTableBuilder::defineAssociatedTypeWitnessTableAccessFunction(
   AssociatedConformance requirement,
   CanType associatedType,
   InterfaceConformanceRef associatedConformance) {
   bool hasArchetype = associatedType->hasArchetype();
   OpaqueTypeArchetypeType *associatedRootOpaqueType = nullptr;
   if (auto assocArchetype = dyn_cast<ArchetypeType>(associatedType)) {
      associatedRootOpaqueType = dyn_cast<OpaqueTypeArchetypeType>(
         assocArchetype->getRoot());
   }

   assert(isa<NormalInterfaceConformance>(Conformance) && "has associated type");

   // Emit an access function.
   llvm::Function *accessor =
      IGM.getAddrOfAssociatedTypeWitnessTableAccessFunction(
         cast<NormalInterfaceConformance>(&Conformance),
         requirement);

   IRGenFunction IGF(IGM, accessor);
   if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, accessor);

   if (IGM.getOptions().optimizeForSize())
      accessor->addFnAttr(llvm::Attribute::NoInline);

   Explosion parameters = IGF.collectParameters();

   llvm::Value *associatedTypeMetadata = parameters.claimNext();

   // We use a non-standard name for the type that states the association
   // requirement rather than the concrete type.
   if (IGM.EnableValueNames) {
      SmallString<128> name;
      name += ConcreteType->getString();
      buildAssociatedTypeValueName(requirement.getAssociation(), name);
      associatedTypeMetadata->setName(name);
   }

   llvm::Value *self = parameters.claimNext();
   setTypeMetadataName(IGM, self, ConcreteType);

   Address destTable(parameters.claimNext(), IGM.getPointerAlignment());
   setInterfaceWitnessTableName(IGM, destTable.getAddress(), ConcreteType,
                                Conformance.getInterface());

   InterfaceDecl *associatedInterface = requirement.getAssociatedRequirement();

   const ConformanceInfo *conformanceI = nullptr;

   if (associatedConformance.isConcrete()) {
      assert(associatedType->isEqual(associatedConformance.getConcrete()->getType()));

      conformanceI = &IGM.getConformanceInfo(associatedInterface,
                                             associatedConformance.getConcrete());

      // If we can emit a constant table, do so.
      if (auto constantTable =
         conformanceI->tryGetConstantTable(IGM, associatedType)) {
         IGF.Builder.CreateRet(constantTable);
         return;
      }
   }

   // If there are no archetypes, return a reference to the table.
   if (!hasArchetype && !associatedRootOpaqueType) {
      auto wtable = conformanceI->getTable(IGF, &associatedTypeMetadata);
      IGF.Builder.CreateRet(wtable);
      return;
   }

   IGF.bindLocalTypeDataFromSelfWitnessTable(
      &Conformance,
      destTable.getAddress(),
      [&](CanType type) {
         return Conformance.getDeclContext()->mapTypeIntoContext(type)
            ->getCanonicalType();
      });

   // If the witness table is directly fulfillable from the type, do so.
   if (auto fulfillment =
      getFulfillmentMap().getWitnessTable(associatedType,
                                          associatedInterface)) {
      // We don't know that 'self' is any better than an abstract metadata here.
      auto source = MetadataResponse::forBounded(self, MetadataState::Abstract);

      llvm::Value *wtable =
         fulfillment->Path.followFromTypeMetadata(IGF, ConcreteType, source,
                                                  MetadataState::Complete,
               /*cache*/ nullptr)
            .getMetadata();
      IGF.Builder.CreateRet(wtable);
      return;
   }

   // Bind local type data from the metadata arguments.
   IGF.bindLocalTypeDataFromTypeMetadata(associatedType, IsExact,
                                         associatedTypeMetadata,
                                         MetadataState::Abstract);
   IGF.bindLocalTypeDataFromTypeMetadata(ConcreteType, IsExact, self,
                                         MetadataState::Abstract);

   // Find abstract conformances.
   // TODO: provide an API to find the best metadata path to the conformance
   // and decide whether it's expensive enough to be worth caching.
   if (!conformanceI) {
      assert(associatedConformance.isAbstract());
      auto wtable =
         emitArchetypeWitnessTableRef(IGF, cast<ArchetypeType>(associatedType),
                                      associatedConformance.getAbstract());
      IGF.Builder.CreateRet(wtable);
      return;
   }

   // Handle concrete conformances involving archetypes.
   auto wtable = conformanceI->getTable(IGF, &associatedTypeMetadata);
   IGF.Builder.CreateRet(wtable);
}

void WitnessTableBuilder::collectResilientWitnesses(
   SmallVectorImpl<llvm::Constant *> &resilientWitnesses) {
   if (!ResilientConformance)
      return;

   assert(isa<NormalInterfaceConformance>(Conformance) &&
          "resilient conformance should always be normal");
   auto &conformance = cast<NormalInterfaceConformance>(Conformance);

   assert(resilientWitnesses.empty());
   for (auto &entry : PILWT->getEntries()) {
      // Associated type witness.
      if (entry.getKind() == PILWitnessTable::AssociatedType) {
         // Associated type witness.
         auto assocType = entry.getAssociatedTypeWitness().Requirement;
         auto associate = conformance.getTypeWitness(assocType);

         llvm::Constant *witness =
            IGM.getAssociatedTypeWitness(associate, /*inInterfaceContext=*/false);
         resilientWitnesses.push_back(witness);
         continue;
      }

      // Associated conformance access function.
      if (entry.getKind() == PILWitnessTable::AssociatedTypeInterface) {
         const auto &witness = entry.getAssociatedTypeInterfaceWitness();

         auto associate =
            ConformanceInContext.getAssociatedType(
               witness.Requirement)->getCanonicalType();

         InterfaceConformanceRef associatedConformance =
            ConformanceInContext.getAssociatedConformance(witness.Requirement,
                                                          witness.Interface);
         AssociatedConformance requirement(PILWT->getInterface(),
                                           witness.Requirement,
                                           witness.Interface);

         llvm::Constant *witnessEntry =
            getAssociatedConformanceWitness(requirement, associate,
                                            associatedConformance);
         resilientWitnesses.push_back(witnessEntry);
         continue;
      }

      // Inherited conformance witnesses.
      if (entry.getKind() == PILWitnessTable::BaseInterface) {
         const auto &witness = entry.getBaseInterfaceWitness();
         auto baseProto = witness.Requirement;
         auto proto = PILWT->getInterface();
         CanType selfType = proto->getInterfaceSelfType()->getCanonicalType();
         AssociatedConformance requirement(proto, selfType, baseProto);
         InterfaceConformanceRef inheritedConformance =
            ConformanceInContext.getAssociatedConformance(selfType, baseProto);
         llvm::Constant *witnessEntry =
            getAssociatedConformanceWitness(requirement, ConcreteType,
                                            inheritedConformance);
         resilientWitnesses.push_back(witnessEntry);
         continue;
      }

      if (entry.getKind() != PILWitnessTable::Method)
         continue;

      PILFunction *Func = entry.getMethodWitness().Witness;
      llvm::Constant *witness;
      if (Func) {
         witness = IGM.getAddrOfPILFunction(Func, NotForDefinition);
      } else {
         // The method is removed by dead method elimination.
         // It should be never called. We add a null pointer.
         witness = nullptr;
      }
      resilientWitnesses.push_back(witness);
   }
}

llvm::Constant *WitnessTableBuilder::buildInstantiationFunction() {
   // We need an instantiation function if any base conformance
   // is non-dependent.
   if (SpecializedBaseConformances.empty())
      return nullptr;

   assert(isa<NormalInterfaceConformance>(Conformance) &&
          "self-conformance requiring instantiation function?");

   llvm::Function *fn =
      IGM.getAddrOfGenericWitnessTableInstantiationFunction(
         cast<NormalInterfaceConformance>(&Conformance));
   IRGenFunction IGF(IGM, fn);
   if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, fn);

   auto PointerAlignment = IGM.getPointerAlignment();
   auto PointerSize = IGM.getPointerSize();

   // Break out the parameters.
   Explosion params = IGF.collectParameters();
   Address wtable(params.claimNext(), PointerAlignment);
   llvm::Value *metadata = params.claimNext();
   IGF.bindLocalTypeDataFromTypeMetadata(ConcreteType, IsExact, metadata,
                                         MetadataState::Complete);
   llvm::Value *instantiationArgs = params.claimNext();
   Address conditionalTables(
      IGF.Builder.CreateBitCast(instantiationArgs,
                                IGF.IGM.WitnessTablePtrPtrTy),
      PointerAlignment);

   // Register local type data for the conditional conformance witness tables.
   for (auto idx : indices(ConditionalRequirementPrivateDataIndices)) {
      Address conditionalTablePtr =
         IGF.Builder.CreateConstArrayGEP(conditionalTables, idx, PointerSize);
      auto conditionalTable = IGF.Builder.CreateLoad(conditionalTablePtr);

      const auto &condConformance = PILConditionalConformances[idx];
      CanType reqTypeInContext =
         Conformance.getDeclContext()
            ->mapTypeIntoContext(condConformance.Requirement)
            ->getCanonicalType();
      if (auto archetype = dyn_cast<ArchetypeType>(reqTypeInContext)) {
         auto condProto = condConformance.Conformance.getRequirement();
         IGF.setUnscopedLocalTypeData(
            archetype,
            LocalTypeDataKind::forAbstractInterfaceWitnessTable(condProto),
            conditionalTable);
      }
   }

   // Initialize all the specialized base conformances.
   for (auto &base : SpecializedBaseConformances) {
      // Ask the ConformanceInfo to emit the wtable.
      llvm::Value *baseWTable =
         base.second->getTable(IGF, &metadata);
      baseWTable = IGF.Builder.CreateBitCast(baseWTable, IGM.Int8PtrTy);

      // Store that to the appropriate slot in the new witness table.
      Address slot =
         IGF.Builder.CreateConstArrayGEP(wtable, base.first, PointerSize);
      IGF.Builder.CreateStore(baseWTable, slot);
   }


   IGF.Builder.CreateRetVoid();

   return fn;
}

namespace {
/// Builds a protocol conformance descriptor.
class InterfaceConformanceDescriptorBuilder {
   IRGenModule &IGM;
   ConstantStructBuilder &B;
   const RootInterfaceConformance *Conformance;
   PILWitnessTable *PILWT;
   ConformanceDescription Description;
   ConformanceFlags Flags;

public:
   InterfaceConformanceDescriptorBuilder(
      IRGenModule &IGM,
      ConstantStructBuilder &B,
      const ConformanceDescription &description)
      : IGM(IGM), B(B), Conformance(description.conformance),
        PILWT(description.wtable), Description(description) { }

   void layout() {
      addInterface();
      addConformingType();
      addWitnessTable();
      addFlags();
      addContext();
      addConditionalRequirements();
      addResilientWitnesses();
      addGenericWitnessTable();

      B.suggestType(IGM.InterfaceConformanceDescriptorTy);
   }

   void addInterface() {
      // Relative reference to the protocol descriptor.
      auto protocol = Conformance->getInterface();
      auto descriptorRef = IGM.getAddrOfLLVMVariableOrGOTEquivalent(
         LinkEntity::forInterfaceDescriptor(protocol));
      B.addRelativeAddress(descriptorRef);
   }

   void addConformingType() {
      // Add a relative reference to the type, with the type reference
      // kind stored in the flags.
      auto ref = IGM.getTypeEntityReference(
         Conformance->getType()->getAnyNominal());
      B.addRelativeAddress(ref.getValue());
      Flags = Flags.withTypeReferenceKind(ref.getKind());
   }

   void addWitnessTable() {
      // Note the number of conditional requirements.
      unsigned numConditional = 0;
      if (auto normal = dyn_cast<NormalInterfaceConformance>(Conformance)) {
         numConditional = normal->getConditionalRequirements().size();
      }
      Flags = Flags.withNumConditionalRequirements(numConditional);

      // Relative reference to the witness table.
      B.addRelativeAddressOrNull(Description.pattern);
   }

   void addFlags() {
      // Miscellaneous flags.
      if (auto conf = dyn_cast<NormalInterfaceConformance>(Conformance)) {
         Flags = Flags.withIsRetroactive(conf->isRetroactive());
         Flags = Flags.withIsSynthesizedNonUnique(conf->isSynthesizedNonUnique());
      } else {
         Flags = Flags.withIsRetroactive(false)
            .withIsSynthesizedNonUnique(false);
      }
      Flags = Flags.withHasResilientWitnesses(
         !Description.resilientWitnesses.empty());
      Flags =
         Flags.withHasGenericWitnessTable(Description.requiresSpecialization);

      // Add the flags.
      B.addInt32(Flags.getIntValue());
   }

   void addContext() {
      auto normal = dyn_cast<NormalInterfaceConformance>(Conformance);
      if (!normal || !normal->isRetroactive())
         return;

      auto moduleContext =
         normal->getDeclContext()->getModuleScopeContext();
      ConstantReference moduleContextRef =
         IGM.getAddrOfParentContextDescriptor(moduleContext,
            /*fromAnonymousContext=*/false);
      B.addRelativeAddress(moduleContextRef);
   }

   void addConditionalRequirements() {
      auto normal = dyn_cast<NormalInterfaceConformance>(Conformance);
      if (!normal || normal->getConditionalRequirements().empty())
         return;

      auto nominal = normal->getType()->getAnyNominal();
      irgen::addGenericRequirements(IGM, B,
                                    nominal->getGenericSignatureOfContext(),
                                    normal->getConditionalRequirements());
   }

   void addResilientWitnesses() {
      if (Description.resilientWitnesses.empty())
         return;

      // TargetResilientWitnessesHeader
      ArrayRef<llvm::Constant *> witnesses = Description.resilientWitnesses;
      B.addInt32(witnesses.size());
      for (const auto &entry : PILWT->getEntries()) {
         // Add the requirement descriptor.
         if (entry.getKind() == PILWitnessTable::AssociatedType) {
            // Associated type descriptor.
            auto assocType = entry.getAssociatedTypeWitness().Requirement;
            auto assocTypeDescriptor =
               IGM.getAddrOfLLVMVariableOrGOTEquivalent(
                  LinkEntity::forAssociatedTypeDescriptor(assocType));
            B.addRelativeAddress(assocTypeDescriptor);
         } else if (entry.getKind() == PILWitnessTable::AssociatedTypeInterface) {
            // Associated conformance descriptor.
            const auto &witness = entry.getAssociatedTypeInterfaceWitness();

            AssociatedConformance requirement(PILWT->getInterface(),
                                              witness.Requirement,
                                              witness.Interface);
            auto assocConformanceDescriptor =
               IGM.getAddrOfLLVMVariableOrGOTEquivalent(
                  LinkEntity::forAssociatedConformanceDescriptor(requirement));
            B.addRelativeAddress(assocConformanceDescriptor);
         } else if (entry.getKind() == PILWitnessTable::BaseInterface) {
            // Associated conformance descriptor for a base protocol.
            const auto &witness = entry.getBaseInterfaceWitness();
            auto proto = PILWT->getInterface();
            BaseConformance requirement(proto, witness.Requirement);
            auto baseConformanceDescriptor =
               IGM.getAddrOfLLVMVariableOrGOTEquivalent(
                  LinkEntity::forBaseConformanceDescriptor(requirement));
            B.addRelativeAddress(baseConformanceDescriptor);
         } else if (entry.getKind() == PILWitnessTable::Method) {
            // Method descriptor.
            auto declRef = entry.getMethodWitness().Requirement;
            auto requirement =
               IGM.getAddrOfLLVMVariableOrGOTEquivalent(
                  LinkEntity::forMethodDescriptor(declRef));
            B.addRelativeAddress(requirement);
         } else {
            // Not part of the resilient witness table.
            continue;
         }

         // Add the witness.
         B.addRelativeAddress(witnesses.front());
         witnesses = witnesses.drop_front();
      }
      assert(witnesses.empty() && "Wrong # of resilient witnesses");
   }

   void addGenericWitnessTable() {
      if (!Description.requiresSpecialization)
         return;

      // WitnessTableSizeInWords
      B.addInt(IGM.Int16Ty, Description.witnessTableSize);
      // WitnessTablePrivateSizeInWordsAndRequiresInstantiation
      B.addInt(IGM.Int16Ty,
               (Description.witnessTablePrivateSize << 1) |
               Description.requiresSpecialization);
      // Instantiation function
      B.addRelativeAddressOrNull(Description.instantiationFn);
      // Private data
      {
         auto privateDataTy =
            llvm::ArrayType::get(IGM.Int8PtrTy,
                                 polar::NumGenericMetadataPrivateDataWords);
         auto privateDataInit = llvm::Constant::getNullValue(privateDataTy);
         auto privateData =
            new llvm::GlobalVariable(IGM.Module, privateDataTy,
               /*constant*/ false,
                                     llvm::GlobalVariable::InternalLinkage,
                                     privateDataInit, "");
         B.addRelativeAddress(privateData);
      }
   }
};
}

void IRGenModule::emitInterfaceConformance(
   const ConformanceDescription &record) {
   auto conformance = record.conformance;

   // Emit additional metadata to be used by reflection.
   emitAssociatedTypeMetadataRecord(conformance);

   // Form the protocol conformance descriptor.
   ConstantInitBuilder initBuilder(*this);
   auto init = initBuilder.beginStruct();
   InterfaceConformanceDescriptorBuilder builder(*this, init, record);
   builder.layout();

   auto var =
      cast<llvm::GlobalVariable>(
         getAddrOfInterfaceConformanceDescriptor(conformance,
                                                 init.finishAndCreateFuture()));
   var->setConstant(true);
   setTrueConstGlobal(var);
}

void IRGenerator::ensureRelativeSymbolCollocation(PILWitnessTable &wt) {
   if (!CurrentIGM)
      return;

   // Only resilient conformances use relative pointers for witness methods.
   if (wt.isDeclaration() || is_available_externally(wt.getLinkage()) ||
       !CurrentIGM->isResilientConformance(wt.getConformance()))
      return;

   for (auto &entry : wt.getEntries()) {
      if (entry.getKind() != PILWitnessTable::Method)
         continue;
      auto *witness = entry.getMethodWitness().Witness;
      if (witness)
         forceLocalEmitOfLazyFunction(witness);
   }
}

void IRGenerator::ensureRelativeSymbolCollocation(PILDefaultWitnessTable &wt) {
   if (!CurrentIGM)
      return;

   for (auto &entry : wt.getEntries()) {
      if (entry.getKind() != PILWitnessTable::Method)
         continue;
      auto *witness = entry.getMethodWitness().Witness;
      if (witness)
         forceLocalEmitOfLazyFunction(witness);
   }
}

/// Do a memoized witness-table layout for a protocol.
const InterfaceInfo &IRGenModule::getInterfaceInfo(InterfaceDecl *protocol,
                                                   InterfaceInfoKind kind) {
   // If the protocol is resilient, we cannot know the full witness table layout.
   assert(!isResilient(protocol, ResilienceExpansion::Maximal) ||
          kind == InterfaceInfoKind::RequirementSignature);

   return Types.getInterfaceInfo(protocol, kind);
}

/// Do a memoized witness-table layout for a protocol.
const InterfaceInfo &TypeConverter::getInterfaceInfo(InterfaceDecl *protocol,
                                                     InterfaceInfoKind kind) {
   // Check whether we've already translated this protocol.
   auto it = Interfaces.find(protocol);
   if (it != Interfaces.end() && it->getSecond()->getKind() >= kind)
      return *it->getSecond();

   // If not, lay out the protocol's witness table, if it needs one.
   WitnessTableLayout layout(kind);
   if (lowering::TypeConverter::interfaceRequiresWitnessTable(protocol))
      layout.visitInterfaceDecl(protocol);

   // Create a InterfaceInfo object from the layout.
   std::unique_ptr<InterfaceInfo> info = InterfaceInfo::create(layout.getEntries(),
                                                               kind);

   // Verify that we haven't generated an incompatible layout.
   if (it != Interfaces.end()) {
      ArrayRef<WitnessTableEntry> originalEntries =
         it->second->getWitnessEntries();
      ArrayRef<WitnessTableEntry> newEntries = info->getWitnessEntries();
      assert(newEntries.size() >= originalEntries.size());
      assert(newEntries.take_front(originalEntries.size()) == originalEntries);
      (void)originalEntries;
      (void)newEntries;
   }

   // Memoize.
   std::unique_ptr<const InterfaceInfo> &cachedInfo = Interfaces[protocol];
   cachedInfo = std::move(info);

   // Done.
   return *cachedInfo;
}

/// Allocate a new InterfaceInfo.
std::unique_ptr<InterfaceInfo>
InterfaceInfo::create(ArrayRef<WitnessTableEntry> table, InterfaceInfoKind kind) {
   size_t bufferSize = totalSizeToAlloc<WitnessTableEntry>(table.size());
   void *buffer = ::operator new(bufferSize);
   return std::unique_ptr<InterfaceInfo>(new(buffer) InterfaceInfo(table, kind));
}

// Provide a unique home for the ConformanceInfo vtable.
void ConformanceInfo::anchor() {}

/// Find the conformance information for a protocol.
const ConformanceInfo &
IRGenModule::getConformanceInfo(const InterfaceDecl *protocol,
                                const InterfaceConformance *conformance) {
   assert(conformance->getInterface() == protocol &&
          "conformance is for wrong protocol");

   auto checkCache =
      [this](const InterfaceConformance *conf) -> const ConformanceInfo * {
         // Check whether we've already cached this.
         auto it = Conformances.find(conf);
         if (it != Conformances.end())
            return it->second.get();

         return nullptr;
      };

   if (auto found = checkCache(conformance))
      return *found;

   //  Drill down to the root normal
   auto rootConformance = conformance->getRootConformance();

   const ConformanceInfo *info;
   // If the conformance is dependent in any way, we need to unique it.
   //
   // FIXME: Both implementations of ConformanceInfo are trivially-destructible,
   // so in theory we could allocate them on a BumpPtrAllocator. But there's not
   // a good one for us to use. (The ASTContext's outlives the IRGenModule in
   // batch mode.)
   if (isDependentConformance(rootConformance) ||
       // Foreign types need to go through the accessor to unique the witness
       // table.
       isSynthesizedNonUnique(rootConformance)) {
      info = new AccessorConformanceInfo(conformance);
      Conformances.try_emplace(conformance, info);
   } else {
      // Otherwise, we can use a direct-referencing conformance, which can get
      // away with the non-specialized conformance.
      if (auto found = checkCache(rootConformance))
         return *found;

      info = new DirectConformanceInfo(rootConformance);
      Conformances.try_emplace(rootConformance, info);
   }

   return *info;
}

/// Whether the witness table will be constant.
static bool isConstantWitnessTable(PILWitnessTable *wt) {
   for (const auto &entry : wt->getEntries()) {
      switch (entry.getKind()) {
         case PILWitnessTable::Invalid:
         case PILWitnessTable::BaseInterface:
         case PILWitnessTable::Method:
            continue;

         case PILWitnessTable::AssociatedType:
         case PILWitnessTable::AssociatedTypeInterface:
            // Associated types and conformances are cached in the witness table.
            // FIXME: If we start emitting constant references to here,
            // we will need to ask the witness table builder for this information.
            return false;
      }
   }

   return true;
}

void IRGenModule::emitPILWitnessTable(PILWitnessTable *wt) {
   // Don't emit a witness table if it is a declaration.
   if (wt->isDeclaration())
      return;

   // Don't emit a witness table that is available externally.
   // It can end up in having duplicate symbols for generated associated type
   // metadata access functions.
   // Also, it is not a big benefit for LLVM to emit such witness tables.
   if (is_available_externally(wt->getLinkage()))
      return;

   // Ensure that relatively-referenced symbols for witness thunks are collocated
   // in the same LLVM module.
   IRGen.ensureRelativeSymbolCollocation(*wt);

   auto conf = wt->getConformance();
   PrettyStackTraceConformance _st(Context, "emitting witness table for", conf);

   // Build the witness table.
   ConstantInitBuilder builder(*this);
   auto wtableContents = builder.beginArray(Int8PtrTy);
   WitnessTableBuilder wtableBuilder(*this, wtableContents, wt);
   wtableBuilder.build();

   SmallVector<llvm::Constant *, 4> resilientWitnesses;
   // Collect the resilient witnesses to go into the conformance descriptor.
   wtableBuilder.collectResilientWitnesses(resilientWitnesses);

   // Produce the initializer value.
   auto initializer = wtableContents.finishAndCreateFuture();

   bool isDependent = isDependentConformance(conf);

   llvm::GlobalVariable *global = nullptr;
   unsigned tableSize;
   if (!isResilientConformance(conf)) {
      global = cast<llvm::GlobalVariable>(
         (isDependent && conf->getDeclContext()->isGenericContext())
         ? getAddrOfWitnessTablePattern(cast<NormalInterfaceConformance>(conf),
                                        initializer)
         : getAddrOfWitnessTable(conf, initializer));
      global->setConstant(isConstantWitnessTable(wt));
      global->setAlignment(getWitnessTableAlignment().getValue());
      tableSize = wtableBuilder.getTableSize();
   } else {
      initializer.abandon();
      tableSize = 0;
   }

   // Collect the information that will go into the protocol conformance
   // descriptor.
   ConformanceDescription description(conf, wt, global, tableSize,
                                      wtableBuilder.getTablePrivateSize(),
                                      isDependent);

   // Build the instantiation function, we if need one.
   description.instantiationFn = wtableBuilder.buildInstantiationFunction();
   description.resilientWitnesses = std::move(resilientWitnesses);

   // Record this conformance descriptor.
   addInterfaceConformance(std::move(description));

   IRGen.noteUseOfTypeContextDescriptor(conf->getType()->getAnyNominal(),
                                        RequireMetadata);
}

/// True if a function's signature in LLVM carries polymorphic parameters.
/// Generic functions and protocol witnesses carry polymorphic parameters.
bool irgen::hasPolymorphicParameters(CanPILFunctionType ty) {
   switch (ty->getRepresentation()) {
      case PILFunctionTypeRepresentation::Block:
         // Should never be polymorphic.
         assert(!ty->isPolymorphic() && "polymorphic C function?!");
         return false;

      case PILFunctionTypeRepresentation::Thick:
      case PILFunctionTypeRepresentation::Thin:
      case PILFunctionTypeRepresentation::Method:
      case PILFunctionTypeRepresentation::Closure:
         return ty->isPolymorphic();

      case PILFunctionTypeRepresentation::CFunctionPointer:
      case PILFunctionTypeRepresentation::ObjCMethod:
         // May be polymorphic at the PIL level, but no type metadata is actually
         // passed.
         return false;

      case PILFunctionTypeRepresentation::WitnessMethod:
         // Always carries polymorphic parameters for the Self type.
         return true;
   }

   llvm_unreachable("Not a valid PILFunctionTypeRepresentation.");
}

/// Emit a polymorphic parameters clause, binding all the metadata necessary.
void EmitPolymorphicParameters::emit(Explosion &in,
                                     WitnessMetadata *witnessMetadata,
                                     const GetParameterFn &getParameter) {
   // Collect any early sources and bind local type data from them.
   for (auto &source : getSources()) {
      bindExtraSource(source, in, witnessMetadata);
   }

   auto getInContext = [&](CanType type) -> CanType {
      return getTypeInContext(type);
   };

   // Collect any concrete type metadata that's been passed separately.
   enumerateUnfulfilledRequirements([&](GenericRequirement requirement) {
      auto value = in.claimNext();
      bindGenericRequirement(IGF, requirement, value, MetadataState::Complete,
                             getInContext);
   });

   // Bind all the fulfillments we can from the formal parameters.
   bindParameterSources(getParameter);
}

MetadataResponse
MetadataPath::followFromTypeMetadata(IRGenFunction &IGF,
                                     CanType sourceType,
                                     MetadataResponse source,
                                     DynamicMetadataRequest request,
                                     Map<MetadataResponse> *cache) const {
   LocalTypeDataKey key = {
      sourceType,
      LocalTypeDataKind::forFormalTypeMetadata()
   };
   return follow(IGF, key, source, Path.begin(), Path.end(), request, cache);
}

MetadataResponse
MetadataPath::followFromWitnessTable(IRGenFunction &IGF,
                                     CanType conformingType,
                                     InterfaceConformanceRef conformance,
                                     MetadataResponse source,
                                     DynamicMetadataRequest request,
                                     Map<MetadataResponse> *cache) const {
   LocalTypeDataKey key = {
      conformingType,
      LocalTypeDataKind::forInterfaceWitnessTable(conformance)
   };
   return follow(IGF, key, source, Path.begin(), Path.end(), request, cache);
}

/// Follow this metadata path.
///
/// \param sourceKey - A description of the source value.  Not necessarily
///   an appropriate caching key.
/// \param cache - If given, this cache will be used to short-circuit
///   the lookup; otherwise, the global (but dominance-sensitive) cache
///   in the IRGenFunction will be used.  This caching system is somewhat
///   more efficient than what IGF provides, but it's less general, and it
///   should probably be removed.
MetadataResponse MetadataPath::follow(IRGenFunction &IGF,
                                      LocalTypeDataKey sourceKey,
                                      MetadataResponse source,
                                      iterator begin, iterator end,
                                      DynamicMetadataRequest finalRequest,
                                      Map<MetadataResponse> *cache) {
   assert(source && "no source metadata value!");

   // The invariant is that this iterator starts a path from source and
   // that sourceKey is correctly describes it.
   iterator i = begin;

   // Before we begin emitting code to generate the actual path, try to find
   // the latest point in the path that we've cached a value for.

   // If the caller gave us a cache to use, check that.  This lookup is very
   // efficient and doesn't even require us to parse the prefix.
   if (cache) {
      auto result = cache->findPrefix(begin, end);
      if (result.first) {
         source = *result.first;

         // If that was the end, there's no more work to do; don't bother
         // adjusting the source key.
         if (result.second == end)
            return source;

         // Advance the source key past the cached prefix.
         while (i != result.second) {
            Component component = *i++;
            (void) followComponent(IGF, sourceKey, MetadataResponse(), component,
                                   MetadataState::Abstract);
         }
      }

      // Otherwise, make a pass over the path looking for available concrete
      // entries in the IGF's local type data cache.
   } else {
      auto skipI = i;
      LocalTypeDataKey skipKey = sourceKey;
      while (skipI != end) {
         Component component = *skipI++;
         (void) followComponent(IGF, skipKey, MetadataResponse(), component,
                                MetadataState::Abstract);

         // Check the cache for a concrete value.  We don't want an abstract
         // cache entry because, if one exists, we'll just end up here again
         // recursively.
         auto skipRequest =
            (skipI == end ? finalRequest : MetadataState::Abstract);
         if (auto skipResponse =
            IGF.tryGetConcreteLocalTypeData(skipKey, skipRequest)) {
            // Advance the baseline information for the source to the current
            // point in the path, then continue the search.
            sourceKey = skipKey;
            source = skipResponse;
            i = skipI;
         }
      }
   }

   // Drill in on the actual source value.
   while (i != end) {
      auto component = *i++;

      auto componentRequest =
         (i == end ? finalRequest : MetadataState::Abstract);
      source = followComponent(IGF, sourceKey, source,
                               component, componentRequest);

      // If we have a cache, remember this in the cache at the next position.
      if (cache) {
         cache->insertNew(begin, i, source);

         // Otherwise, insert it into the global cache (at the updated source key).
      } else {
         IGF.setScopedLocalTypeData(sourceKey, source);
      }
   }

   return source;
}

/// Call an associated-type witness table access function.  Does not do
/// any caching or drill down to implied protocols.
static llvm::Value *
emitAssociatedTypeWitnessTableRef(IRGenFunction &IGF,
                                  llvm::Value *parentMetadata,
                                  llvm::Value *wtable,
                                  AssociatedConformance conformance,
                                  llvm::Value *associatedTypeMetadata) {
   auto sourceInterface = conformance.getSourceInterface();
   auto assocConformanceDescriptor =
      IGF.IGM.getAddrOfAssociatedConformanceDescriptor(conformance);
   auto baseDescriptor =
      IGF.IGM.getAddrOfInterfaceRequirementsBaseDescriptor(sourceInterface);

   auto call =
      IGF.Builder.CreateCall(IGF.IGM.getGetAssociatedConformanceWitnessFn(),
                             {
                                wtable, parentMetadata,
                                associatedTypeMetadata,
                                baseDescriptor, assocConformanceDescriptor
                             });
   call->setDoesNotThrow();
   call->setDoesNotAccessMemory();
   return call;
}

/// Drill down on a single stage of component.
///
/// sourceType and sourceDecl will be adjusted to refer to the new
/// component.  Source can be null, in which case this will be the only
/// thing done.
MetadataResponse MetadataPath::followComponent(IRGenFunction &IGF,
                                               LocalTypeDataKey &sourceKey,
                                               MetadataResponse source,
                                               Component component,
                                               DynamicMetadataRequest request) {
   switch (component.getKind()) {
      case Component::Kind::NominalTypeArgument:
      case Component::Kind::NominalTypeArgumentConformance: {
         assert(sourceKey.Kind == LocalTypeDataKind::forFormalTypeMetadata());
         auto type = sourceKey.Type;
         if (auto archetypeTy = dyn_cast<ArchetypeType>(type))
            type = archetypeTy->getSuperclass()->getCanonicalType();
         auto *nominal = type.getAnyNominal();
         auto reqtIndex = component.getPrimaryIndex();

         GenericTypeRequirements requirements(IGF.IGM, nominal);
         auto &requirement = requirements.getRequirements()[reqtIndex];

         auto module = IGF.getSwiftModule();
         auto subs = sourceKey.Type->getContextSubstitutionMap(module, nominal);
         auto sub = requirement.TypeParameter.subst(subs)->getCanonicalType();

         // In either case, we need to change the type.
         sourceKey.Type = sub;

         // If this is a type argument, we've fully updated sourceKey.
         if (component.getKind() == Component::Kind::NominalTypeArgument) {
            assert(!requirement.Interface && "index mismatch!");

            if (!source) return MetadataResponse();

            auto sourceMetadata = source.getMetadata();
            auto *argMetadata =
               emitArgumentMetadataRef(IGF, nominal, requirements, reqtIndex,
                                       sourceMetadata);
            setTypeMetadataName(IGF.IGM, argMetadata, sourceKey.Type);

            // Assume that the argument metadata is complete if the metadata is.
            auto argState = getPresumedMetadataStateForTypeArgument(
               source.getStaticLowerBoundOnState());
            auto response = MetadataResponse::forBounded(argMetadata, argState);

            // Do a dynamic check if necessary to satisfy the request.
            return emitCheckTypeMetadataState(IGF, request, response);

            // Otherwise, we need to switch sourceKey.Kind to the appropriate
            // conformance kind.
         } else {
            assert(requirement.Interface && "index mismatch!");
            auto conformance = subs.lookupConformance(requirement.TypeParameter,
                                                      requirement.Interface);
            assert(conformance.getRequirement() == requirement.Interface);
            sourceKey.Kind = LocalTypeDataKind::forInterfaceWitnessTable(conformance);

            if (!source) return MetadataResponse();

            auto sourceMetadata = source.getMetadata();
            auto protocol = conformance.getRequirement();
            auto wtable = emitArgumentWitnessTableRef(IGF, nominal,
                                                      requirements, reqtIndex,
                                                      sourceMetadata);
            setInterfaceWitnessTableName(IGF.IGM, wtable, sourceKey.Type, protocol);

            return MetadataResponse::forComplete(wtable);
         }
      }

      case Component::Kind::OutOfLineBaseInterface: {
         auto conformance = sourceKey.Kind.getInterfaceConformance();
         auto protocol = conformance.getRequirement();
         auto &pi = IGF.IGM.getInterfaceInfo(protocol,
                                             InterfaceInfoKind::RequirementSignature);

         auto &entry = pi.getWitnessEntries()[component.getPrimaryIndex()];
         assert(entry.isOutOfLineBase());
         auto inheritedInterface = entry.getBase();

         sourceKey.Kind =
            LocalTypeDataKind::forAbstractInterfaceWitnessTable(inheritedInterface);
         if (conformance.isConcrete()) {
            auto inheritedConformance =
               conformance.getConcrete()->getInheritedConformance(inheritedInterface);
            if (inheritedConformance) {
               sourceKey.Kind = LocalTypeDataKind::forConcreteInterfaceWitnessTable(
                  inheritedConformance);
            }
         }

         if (!source) return MetadataResponse();

         auto wtable = source.getMetadata();
         WitnessIndex index(component.getPrimaryIndex(), /*prefix*/ false);
         auto baseWTable =
            emitInvariantLoadOfOpaqueWitness(IGF, wtable,
                                             index.forInterfaceWitnessTable());
         baseWTable =
            IGF.Builder.CreateBitCast(baseWTable, IGF.IGM.WitnessTablePtrTy);
         setInterfaceWitnessTableName(IGF.IGM, baseWTable, sourceKey.Type,
                                      inheritedInterface);

         return MetadataResponse::forComplete(baseWTable);
      }

      case Component::Kind::AssociatedConformance: {
         auto sourceType = sourceKey.Type;
         auto sourceConformance = sourceKey.Kind.getInterfaceConformance();
         auto sourceInterface = sourceConformance.getRequirement();
         auto &pi = IGF.IGM.getInterfaceInfo(sourceInterface,
                                             InterfaceInfoKind::RequirementSignature);

         auto &entry = pi.getWitnessEntries()[component.getPrimaryIndex()];
         assert(entry.isAssociatedConformance());
         auto association = entry.getAssociatedConformancePath();
         auto associatedRequirement = entry.getAssociatedConformanceRequirement();

         CanType associatedType =
            sourceConformance.getAssociatedType(sourceType, association)
               ->getCanonicalType();
         if (sourceConformance.isConcrete() &&
             isa<NormalInterfaceConformance>(sourceConformance.getConcrete())) {
            associatedType =
               sourceConformance.getConcrete()->getDeclContext()
                  ->mapTypeIntoContext(associatedType)
                  ->getCanonicalType();
         }
         sourceKey.Type = associatedType;

         auto associatedConformance =
            sourceConformance.getAssociatedConformance(sourceType, association,
                                                       associatedRequirement);
         sourceKey.Kind =
            LocalTypeDataKind::forInterfaceWitnessTable(associatedConformance);

         assert((associatedConformance.isConcrete() ||
                 isa<ArchetypeType>(sourceKey.Type)) &&
                "couldn't find concrete conformance for concrete type");

         if (!source) return MetadataResponse();

         auto sourceMetadata = IGF.emitTypeMetadataRef(sourceType);
         auto associatedMetadata = IGF.emitTypeMetadataRef(sourceKey.Type);
         auto sourceWTable = source.getMetadata();

         AssociatedConformance associatedConformanceRef(sourceInterface,
                                                        association,
                                                        associatedRequirement);
         auto associatedWTable =
            emitAssociatedTypeWitnessTableRef(IGF, sourceMetadata, sourceWTable,
                                              associatedConformanceRef,
                                              associatedMetadata);

         setInterfaceWitnessTableName(IGF.IGM, associatedWTable, sourceKey.Type,
                                      associatedRequirement);

         return MetadataResponse::forComplete(associatedWTable);
      }

      case Component::Kind::ConditionalConformance: {
         auto sourceConformance = sourceKey.Kind.getInterfaceConformance();

         auto reqtIndex = component.getPrimaryIndex();

         InterfaceDecl *conformingProto;
         auto found = PILWitnessTable::enumerateWitnessTableConditionalConformances(
            sourceConformance.getConcrete(),
            [&](unsigned index, CanType type, InterfaceDecl *proto) {
               if (reqtIndex == index) {
                  conformingProto = proto;
                  sourceKey.Type = type;
                  // done!
                  return true;
               }
               return /*finished?*/ false;
            });
         assert(found && "too many conditional conformances");
         (void)found;

         sourceKey.Kind =
            LocalTypeDataKind::forAbstractInterfaceWitnessTable(conformingProto);

         if (!source) return MetadataResponse();

         WitnessIndex index(privateWitnessTableIndexToTableOffset(reqtIndex),
            /*prefix*/ false);

         auto sourceWTable = source.getMetadata();
         auto capturedWTable =
            emitInvariantLoadOfOpaqueWitness(IGF, sourceWTable, index);
         capturedWTable =
            IGF.Builder.CreateBitCast(capturedWTable, IGF.IGM.WitnessTablePtrTy);
         setInterfaceWitnessTableName(IGF.IGM, capturedWTable, sourceKey.Type,
                                      conformingProto);

         return MetadataResponse::forComplete(capturedWTable);
      }

      case Component::Kind::Impossible:
         llvm_unreachable("following an impossible path!");

   }
   llvm_unreachable("bad metadata path component");
}

void MetadataPath::dump() const {
   auto &out = llvm::errs();
   print(out);
   out << '\n';
}
void MetadataPath::print(llvm::raw_ostream &out) const {
   for (auto i = Path.begin(), e = Path.end(); i != e; ++i) {
      if (i != Path.begin()) out << ".";
      auto component = *i;
      switch (component.getKind()) {
         case Component::Kind::OutOfLineBaseInterface:
            out << "out_of_line_base_protocol[" << component.getPrimaryIndex() << "]";
            break;
         case Component::Kind::AssociatedConformance:
            out << "associated_conformance[" << component.getPrimaryIndex() << "]";
            break;
         case Component::Kind::NominalTypeArgument:
            out << "nominal_type_argument[" << component.getPrimaryIndex() << "]";
            break;
         case Component::Kind::NominalTypeArgumentConformance:
            out << "nominal_type_argument_conformance["
                << component.getPrimaryIndex() << "]";
            break;
         case Component::Kind::ConditionalConformance:
            out << "conditional_conformance[" << component.getPrimaryIndex() << "]";
            break;
         case Component::Kind::Impossible:
            out << "impossible";
            break;
      }
   }
}

/// Collect any required metadata for a witness method from the end of
/// the given parameter list.
void irgen::collectTrailingWitnessMetadata(IRGenFunction &IGF,
                                           PILFunction &fn,
                                           Explosion &params,
                                           WitnessMetadata &witnessMetadata) {
   assert(fn.getLoweredFunctionType()->getRepresentation()
          == PILFunctionTypeRepresentation::WitnessMethod);

   llvm::Value *wtable = params.takeLast();
   assert(wtable->getType() == IGF.IGM.WitnessTablePtrTy &&
          "parameter signature mismatch: witness metadata didn't "
          "end in witness table?");
   wtable->setName("SelfWitnessTable");
   witnessMetadata.SelfWitnessTable = wtable;

   llvm::Value *metatype = params.takeLast();
   assert(metatype->getType() == IGF.IGM.TypeMetadataPtrTy &&
          "parameter signature mismatch: witness metadata didn't "
          "end in metatype?");
   metatype->setName("Self");
   witnessMetadata.SelfMetadata = metatype;
}

/// Perform all the bindings necessary to emit the given declaration.
void irgen::emitPolymorphicParameters(IRGenFunction &IGF,
                                      PILFunction &Fn,
                                      Explosion &in,
                                      WitnessMetadata *witnessMetadata,
                                      const GetParameterFn &getParameter) {
   EmitPolymorphicParameters(IGF, Fn).emit(in, witnessMetadata, getParameter);
}

/// Given an array of polymorphic arguments as might be set up by
/// GenericArguments, bind the polymorphic parameters.
void irgen::emitPolymorphicParametersFromArray(IRGenFunction &IGF,
                                               NominalTypeDecl *typeDecl,
                                               Address array,
                                               MetadataState state) {
   GenericTypeRequirements requirements(IGF.IGM, typeDecl);

   array = IGF.Builder.CreateElementBitCast(array, IGF.IGM.TypeMetadataPtrTy);

   auto getInContext = [&](CanType type) -> CanType {
      return typeDecl->mapTypeIntoContext(type)
         ->getCanonicalType();
   };

   // Okay, bind everything else from the context.
   requirements.bindFromBuffer(IGF, array, state, getInContext);
}

Size NecessaryBindings::getBufferSize(IRGenModule &IGM) const {
   // We need one pointer for each archetype or witness table.
   return IGM.getPointerSize() * Requirements.size();
}

void NecessaryBindings::restore(IRGenFunction &IGF, Address buffer,
                                MetadataState metadataState) const {
   bindFromGenericRequirementsBuffer(IGF, Requirements.getArrayRef(), buffer,
                                     metadataState,
                                     [&](CanType type) { return type;});
}

void NecessaryBindings::save(IRGenFunction &IGF, Address buffer) const {
   emitInitOfGenericRequirementsBuffer(IGF, Requirements.getArrayRef(), buffer,
                                       [&](GenericRequirement requirement) -> llvm::Value* {
                                          CanType type = requirement.TypeParameter;
                                          if (auto protocol = requirement.Interface) {
                                             auto wtable =
                                                emitArchetypeWitnessTableRef(IGF, cast<ArchetypeType>(type), protocol);
                                             return wtable;
                                          } else {
                                             auto metadata = IGF.emitTypeMetadataRef(type);
                                             return metadata;
                                          }
                                       });
}

void NecessaryBindings::addTypeMetadata(CanType type) {
   assert(!isa<InOutType>(type));

   // Bindings are only necessary at all if the type is dependent.
   if (!type->hasArchetype()) return;

   // Break down structural types so that we don't eagerly pass metadata
   // for the structural type.  Future considerations for this:
   //   - If we have the structural type lying around in some cheap fashion,
   //     maybe we *should* just pass it.
   //   - Passing a structural type should remove the need to pass its
   //     components separately.
   if (auto tuple = dyn_cast<TupleType>(type)) {
      for (auto elt : tuple.getElementTypes())
         addTypeMetadata(elt);
      return;
   }
   if (auto fn = dyn_cast<FunctionType>(type)) {
      for (const auto &elt : fn.getParams())
         addTypeMetadata(elt.getPlainType());
      addTypeMetadata(fn.getResult());
      return;
   }
   if (auto metatype = dyn_cast<MetatypeType>(type)) {
      addTypeMetadata(metatype.getInstanceType());
      return;
   }
   // Generic types are trickier, because they can require conformances.

   // Otherwise, just record the need for this metadata.
   Requirements.insert({type, nullptr});
}

void NecessaryBindings::addInterfaceConformance(CanType type,
                                                InterfaceConformanceRef conf) {
   if (!conf.isAbstract()) return;
   assert(isa<ArchetypeType>(type));

   // TODO: pass something about the root conformance necessary to
   // reconstruct this.
   Requirements.insert({type, conf.getAbstract()});
}

llvm::Value *irgen::emitWitnessTableRef(IRGenFunction &IGF,
                                        CanType srcType,
                                        InterfaceConformanceRef conformance) {
   llvm::Value *srcMetadataCache = nullptr;
   return emitWitnessTableRef(IGF, srcType, &srcMetadataCache, conformance);
}

/// Emit a protocol witness table for a conformance.
llvm::Value *irgen::emitWitnessTableRef(IRGenFunction &IGF,
                                        CanType srcType,
                                        llvm::Value **srcMetadataCache,
                                        InterfaceConformanceRef conformance) {
   auto proto = conformance.getRequirement();
   assert(lowering::TypeConverter::interfaceRequiresWitnessTable(proto)
          && "protocol does not have witness tables?!");

   // Look through any opaque types we're allowed to.
   if (srcType->hasOpaqueArchetype()) {
      std::tie(srcType, conformance) =
         IGF.IGM.substOpaqueTypesWithUnderlyingTypes(srcType, conformance);
   }

   // If we don't have concrete conformance information, the type must be
   // an archetype and the conformance must be via one of the protocol
   // requirements of the archetype. Look at what's locally bound.
   InterfaceConformance *concreteConformance;
   if (conformance.isAbstract()) {
      auto archetype = cast<ArchetypeType>(srcType);
      return emitArchetypeWitnessTableRef(IGF, archetype, proto);

      // All other source types should be concrete enough that we have
      // conformance info for them.  However, that conformance info might be
      // more concrete than we're expecting.
      // TODO: make a best effort to devirtualize, maybe?
   } else {
      concreteConformance = conformance.getConcrete();
   }
   assert(concreteConformance->getInterface() == proto);

   auto cacheKind =
      LocalTypeDataKind::forConcreteInterfaceWitnessTable(concreteConformance);

   // Check immediately for an existing cache entry.
   auto wtable = IGF.tryGetLocalTypeData(srcType, cacheKind);
   if (wtable) return wtable;

   auto &conformanceI = IGF.IGM.getConformanceInfo(proto, concreteConformance);
   wtable = conformanceI.getTable(IGF, srcMetadataCache);

   IGF.setScopedLocalTypeData(srcType, cacheKind, wtable);
   return wtable;
}

static CanType getSubstSelfType(IRGenModule &IGM,
                                CanPILFunctionType origFnType,
                                SubstitutionMap subs) {
   // Grab the apparent 'self' type.  If there isn't a 'self' type,
   // we're not going to try to access this anyway.
   assert(!origFnType->getParameters().empty());

   auto selfParam = origFnType->getParameters().back();
   CanType inputType = selfParam.getArgumentType(IGM.getPILModule(), origFnType);
   // If the parameter is a direct metatype parameter, this is a static method
   // of the instance type. We can assume this because:
   // - metatypes cannot directly conform to protocols
   // - even if they could, they would conform as a value type 'self' and thus
   //   be passed indirectly as an @in or @inout parameter.
   if (auto meta = dyn_cast<MetatypeType>(inputType)) {
      if (!selfParam.isFormalIndirect())
         inputType = meta.getInstanceType();
   }

   // Substitute the `self` type.
   // FIXME: This has to be done as a formal AST type substitution rather than
   // a PIL function type substitution, because some nominal types (viz
   // Optional) have type lowering recursively applied to their type parameters.
   // Substituting into the original lowered function type like this is still
   // problematic if we ever allow methods or protocol conformances on structural
   // types; we'd really need to separately record the formal Self type in the
   // PIL function type to make that work, which could be managed by having a
   // "substituted generic signature" concept.
   if (!subs.empty()) {
      inputType = inputType.subst(subs)->getCanonicalType();
   }

   return inputType;
}

namespace {
class EmitPolymorphicArguments : public PolymorphicConvention {
   IRGenFunction &IGF;
public:
   EmitPolymorphicArguments(IRGenFunction &IGF,
                            CanPILFunctionType polyFn)
      : PolymorphicConvention(IGF.IGM, polyFn), IGF(IGF) {}

   void emit(SubstitutionMap subs,
             WitnessMetadata *witnessMetadata, Explosion &out);

private:
   void emitEarlySources(SubstitutionMap subs, Explosion &out) {
      for (auto &source : getSources()) {
         switch (source.getKind()) {
            // Already accounted for in the parameters.
            case MetadataSource::Kind::ClassPointer:
            case MetadataSource::Kind::Metadata:
               continue;

               // Needs a special argument.
            case MetadataSource::Kind::GenericLValueMetadata: {
               out.add(
                  IGF.emitTypeMetadataRef(getSubstSelfType(IGF.IGM, FnType, subs)));
               continue;
            }

               // Witness 'Self' arguments are added as a special case in
               // EmitPolymorphicArguments::emit.
            case MetadataSource::Kind::SelfMetadata:
            case MetadataSource::Kind::SelfWitnessTable:
               continue;
         }
         llvm_unreachable("bad source kind!");
      }
   }
};
} // end anonymous namespace

/// Pass all the arguments necessary for the given function.
void irgen::emitPolymorphicArguments(IRGenFunction &IGF,
                                     CanPILFunctionType origFnType,
                                     SubstitutionMap subs,
                                     WitnessMetadata *witnessMetadata,
                                     Explosion &out) {
   EmitPolymorphicArguments(IGF, origFnType).emit(subs, witnessMetadata, out);
}

void EmitPolymorphicArguments::emit(SubstitutionMap subs,
                                    WitnessMetadata *witnessMetadata,
                                    Explosion &out) {
   // Add all the early sources.
   emitEarlySources(subs, out);

   // For now, treat all archetypes independently.
   enumerateUnfulfilledRequirements([&](GenericRequirement requirement) {
      llvm::Value *requiredValue =
         emitGenericRequirementFromSubstitutions(IGF, Generics, M,
                                                 requirement, subs);
      out.add(requiredValue);
   });

   // For a witness call, add the Self argument metadata arguments last.
   for (auto &source : getSources()) {
      switch (source.getKind()) {
         case MetadataSource::Kind::Metadata:
         case MetadataSource::Kind::ClassPointer:
            // Already accounted for in the arguments.
            continue;

         case MetadataSource::Kind::GenericLValueMetadata:
            // Added in the early phase.
            continue;

         case MetadataSource::Kind::SelfMetadata: {
            assert(witnessMetadata && "no metadata structure for witness method");
            auto self = IGF.emitTypeMetadataRef(
               getSubstSelfType(IGF.IGM, FnType, subs));
            witnessMetadata->SelfMetadata = self;
            continue;
         }

         case MetadataSource::Kind::SelfWitnessTable: {
            // Added later.
            continue;
         }
      }
      llvm_unreachable("bad source kind");
   }
}

NecessaryBindings
NecessaryBindings::forFunctionInvocations(IRGenModule &IGM,
                                          CanPILFunctionType origType,
                                          SubstitutionMap subs) {
   NecessaryBindings bindings;

   // Bail out early if we don't have polymorphic parameters.
   if (!hasPolymorphicParameters(origType))
      return bindings;

   // Figure out what we're actually required to pass:
   PolymorphicConvention convention(IGM, origType);

   //  - unfulfilled requirements
   convention.enumerateUnfulfilledRequirements(
      [&](GenericRequirement requirement) {
         CanType type = requirement.TypeParameter.subst(subs)->getCanonicalType();

         if (requirement.Interface) {
            auto conf = subs.lookupConformance(requirement.TypeParameter,
                                               requirement.Interface);
            bindings.addInterfaceConformance(type, conf);
         } else {
            bindings.addTypeMetadata(type);
         }
      });

   //   - extra sources
   for (auto &source : convention.getSources()) {
      switch (source.getKind()) {
         case MetadataSource::Kind::Metadata:
         case MetadataSource::Kind::ClassPointer:
            continue;

         case MetadataSource::Kind::GenericLValueMetadata:
            bindings.addTypeMetadata(getSubstSelfType(IGM, origType, subs));
            continue;

         case MetadataSource::Kind::SelfMetadata:
            bindings.addTypeMetadata(getSubstSelfType(IGM, origType, subs));
            continue;

         case MetadataSource::Kind::SelfWitnessTable:
            // We'll just pass undef in cases like this.
            continue;
      }
      llvm_unreachable("bad source kind");
   }

   return bindings;
}

/// The information we need to record in generic type metadata
/// is the information in the type's generic signature.  This is
/// simply the information that would be passed to a generic function
/// that takes the (thick) parent metatype as an argument.
GenericTypeRequirements::GenericTypeRequirements(IRGenModule &IGM,
                                                 NominalTypeDecl *typeDecl)
   : TheDecl(typeDecl) {
   // We only need to do something here if the declaration context is
   // somehow generic.
   auto ncGenerics = typeDecl->getGenericSignatureOfContext();
   if (!ncGenerics || ncGenerics->areAllParamsConcrete()) return;

   // Construct a representative function type.
   auto generics = ncGenerics->getCanonicalSignature();
   auto fnType = PILFunctionType::get(generics, PILFunctionType::ExtInfo(),
                                      PILCoroutineKind::None,
      /*callee*/ ParameterConvention::Direct_Unowned,
      /*params*/ {}, /*yields*/ {},
      /*results*/ {}, /*error*/ None,
      /*subs*/ SubstitutionMap(), /*implied*/ false,
                                      IGM.Context);

   // Figure out what we're actually still required to pass
   PolymorphicConvention convention(IGM, fnType);
   convention.enumerateUnfulfilledRequirements([&](GenericRequirement reqt) {
      assert(generics->isCanonicalTypeInContext(reqt.TypeParameter));
      Requirements.push_back(reqt);
   });

   // We do not need to consider extra sources.
}

void
GenericTypeRequirements::enumerateFulfillments(IRGenModule &IGM,
                                               SubstitutionMap subs,
                                               FulfillmentCallback callback) {
   if (empty()) return;

   for (auto reqtIndex : indices(getRequirements())) {
      auto &reqt = getRequirements()[reqtIndex];
      CanType type = reqt.TypeParameter.subst(subs)->getCanonicalType();
      if (reqt.Interface) {
         auto conformance =
            subs.lookupConformance(reqt.TypeParameter, reqt.Interface);
         callback(reqtIndex, type, conformance);
      } else {
         callback(reqtIndex, type, InterfaceConformanceRef::forInvalid());
      }
   }
}

void GenericTypeRequirements::emitInitOfBuffer(IRGenFunction &IGF,
                                               SubstitutionMap subs,
                                               Address buffer) {
   if (Requirements.empty()) return;

   auto generics =
      TheDecl->getGenericSignatureOfContext()->getCanonicalSignature();
   auto &module = *TheDecl->getParentModule();
   emitInitOfGenericRequirementsBuffer(IGF, Requirements, buffer,
                                       [&](GenericRequirement requirement) {
                                          return emitGenericRequirementFromSubstitutions(IGF, generics, module,
                                                                                         requirement, subs);
                                       });
}

void irgen::emitInitOfGenericRequirementsBuffer(IRGenFunction &IGF,
                                                ArrayRef<GenericRequirement> requirements,
                                                Address buffer,
                                                EmitGenericRequirementFn emitRequirement) {
   if (requirements.empty()) return;

   // Cast the buffer to %type**.
   buffer = IGF.Builder.CreateElementBitCast(buffer, IGF.IGM.TypeMetadataPtrTy);

   for (auto index : indices(requirements)) {
      // GEP to the appropriate slot.
      Address slot = buffer;
      if (index != 0) {
         slot = IGF.Builder.CreateConstArrayGEP(slot, index,
                                                IGF.IGM.getPointerSize());
      }

      llvm::Value *value = emitRequirement(requirements[index]);
      if (requirements[index].Interface) {
         slot = IGF.Builder.CreateElementBitCast(slot, IGF.IGM.WitnessTablePtrTy);
      }
      IGF.Builder.CreateStore(value, slot);
   }
}

llvm::Value *
irgen::emitGenericRequirementFromSubstitutions(IRGenFunction &IGF,
                                               CanGenericSignature generics,
                                               ModuleDecl &module,
                                               GenericRequirement requirement,
                                               SubstitutionMap subs) {
   CanType depTy = requirement.TypeParameter;
   CanType argType = depTy.subst(subs)->getCanonicalType();

   if (!requirement.Interface) {
      auto argMetadata = IGF.emitTypeMetadataRef(argType);
      return argMetadata;
   }

   auto proto = requirement.Interface;
   auto conformance = subs.lookupConformance(depTy, proto);
   assert(conformance.getRequirement() == proto);
   llvm::Value *metadata = nullptr;
   auto wtable = emitWitnessTableRef(IGF, argType, &metadata, conformance);
   return wtable;
}

void GenericTypeRequirements::bindFromBuffer(IRGenFunction &IGF,
                                             Address buffer,
                                             MetadataState metadataState,
                                             GetTypeParameterInContextFn getInContext) {
   bindFromGenericRequirementsBuffer(IGF, Requirements, buffer,
                                     metadataState, getInContext);
}

void irgen::bindFromGenericRequirementsBuffer(IRGenFunction &IGF,
                                              ArrayRef<GenericRequirement> requirements,
                                              Address buffer,
                                              MetadataState metadataState,
                                              GetTypeParameterInContextFn getInContext) {
   if (requirements.empty()) return;

   // Cast the buffer to %type**.
   buffer = IGF.Builder.CreateElementBitCast(buffer, IGF.IGM.TypeMetadataPtrTy);

   for (auto index : indices(requirements)) {
      // GEP to the appropriate slot.
      Address slot = buffer;
      if (index != 0) {
         slot = IGF.Builder.CreateConstArrayGEP(slot, index,
                                                IGF.IGM.getPointerSize());
      }

      // Cast if necessary.
      if (requirements[index].Interface) {
         slot = IGF.Builder.CreateElementBitCast(slot, IGF.IGM.WitnessTablePtrTy);
      }

      llvm::Value *value = IGF.Builder.CreateLoad(slot);
      bindGenericRequirement(IGF, requirements[index], value, metadataState,
                             getInContext);
   }
}

void irgen::bindGenericRequirement(IRGenFunction &IGF,
                                   GenericRequirement requirement,
                                   llvm::Value *value,
                                   MetadataState metadataState,
                                   GetTypeParameterInContextFn getInContext) {
   // Get the corresponding context type.
   auto type = getInContext(requirement.TypeParameter);

   if (auto proto = requirement.Interface) {
      assert(isa<ArchetypeType>(type));
      assert(value->getType() == IGF.IGM.WitnessTablePtrTy);
      setInterfaceWitnessTableName(IGF.IGM, value, type, proto);
      auto kind = LocalTypeDataKind::forAbstractInterfaceWitnessTable(proto);
      IGF.setUnscopedLocalTypeData(type, kind, value);
   } else {
      assert(value->getType() == IGF.IGM.TypeMetadataPtrTy);
      setTypeMetadataName(IGF.IGM, value, type);
      IGF.bindLocalTypeDataFromTypeMetadata(type, IsExact, value, metadataState);
   }
}

namespace {
/// A class for expanding a polymorphic signature.
class ExpandPolymorphicSignature : public PolymorphicConvention {
public:
   ExpandPolymorphicSignature(IRGenModule &IGM, CanPILFunctionType fn)
      : PolymorphicConvention(IGM, fn) {}

   void expand(SmallVectorImpl<llvm::Type*> &out) {
      for (auto &source : getSources())
         addEarlySource(source, out);

      enumerateUnfulfilledRequirements([&](GenericRequirement reqt) {
         out.push_back(reqt.Interface ? IGM.WitnessTablePtrTy
                                      : IGM.TypeMetadataPtrTy);
      });
   }

private:
   /// Add signature elements for the source metadata.
   void addEarlySource(const MetadataSource &source,
                       SmallVectorImpl<llvm::Type*> &out) {
      switch (source.getKind()) {
         case MetadataSource::Kind::ClassPointer: return; // already accounted for
         case MetadataSource::Kind::Metadata: return; // already accounted for
         case MetadataSource::Kind::GenericLValueMetadata:
            return out.push_back(IGM.TypeMetadataPtrTy);
         case MetadataSource::Kind::SelfMetadata:
         case MetadataSource::Kind::SelfWitnessTable:
            return; // handled as a special case in expand()
      }
      llvm_unreachable("bad source kind");
   }
};
} // end anonymous namespace

/// Given a generic signature, add the argument types required in order to call it.
void irgen::expandPolymorphicSignature(IRGenModule &IGM,
                                       CanPILFunctionType polyFn,
                                       SmallVectorImpl<llvm::Type*> &out) {
   ExpandPolymorphicSignature(IGM, polyFn).expand(out);
}

void irgen::expandTrailingWitnessSignature(IRGenModule &IGM,
                                           CanPILFunctionType polyFn,
                                           SmallVectorImpl<llvm::Type*> &out) {
   assert(polyFn->getRepresentation()
          == PILFunctionTypeRepresentation::WitnessMethod);

   assert(getTrailingWitnessSignatureLength(IGM, polyFn) == 2);

   // A witness method always provides Self.
   out.push_back(IGM.TypeMetadataPtrTy);

   // A witness method always provides the witness table for Self.
   out.push_back(IGM.WitnessTablePtrTy);
}

FunctionPointer irgen::emitWitnessMethodValue(IRGenFunction &IGF,
                                              llvm::Value *wtable,
                                              PILDeclRef member) {
   auto *fn = cast<AbstractFunctionDecl>(member.getDecl());
   auto proto = cast<InterfaceDecl>(fn->getDeclContext());

   assert(!IGF.IGM.isResilient(proto, ResilienceExpansion::Maximal));

   // Find the witness we're interested in.
   auto &fnProtoInfo = IGF.IGM.getInterfaceInfo(proto, InterfaceInfoKind::Full);
   auto index = fnProtoInfo.getFunctionIndex(fn);
   llvm::Value *witnessFnPtr =
      emitInvariantLoadOfOpaqueWitness(IGF, wtable,
                                       index.forInterfaceWitnessTable());

   auto fnType = IGF.IGM.getPILTypes().getConstantFunctionType(
      IGF.IGM.getMaximalTypeExpansionContext(), member);
   Signature signature = IGF.IGM.getSignature(fnType);
   witnessFnPtr = IGF.Builder.CreateBitCast(witnessFnPtr,
                                            signature.getType()->getPointerTo());

   return FunctionPointer(witnessFnPtr, signature);
}

FunctionPointer irgen::emitWitnessMethodValue(
   IRGenFunction &IGF, CanType baseTy, llvm::Value **baseMetadataCache,
   PILDeclRef member, InterfaceConformanceRef conformance) {
   llvm::Value *wtable = emitWitnessTableRef(IGF, baseTy, baseMetadataCache,
                                             conformance);

   return emitWitnessMethodValue(IGF, wtable, member);
}

llvm::Value *irgen::computeResilientWitnessTableIndex(
   IRGenFunction &IGF,
   InterfaceDecl *proto,
   llvm::Constant *reqtDescriptor) {
   // The requirement base descriptor refers to the first requirement in the
   // protocol descriptor, offset by the start of the witness table requirements.
   auto requirementsBaseDescriptor =
      IGF.IGM.getAddrOfInterfaceRequirementsBaseDescriptor(proto);

   // Subtract the two pointers to determine the offset to this particular
   // requirement.
   auto baseAddress = IGF.Builder.CreatePtrToInt(requirementsBaseDescriptor,
                                                 IGF.IGM.IntPtrTy);
   auto reqtAddress = IGF.Builder.CreatePtrToInt(reqtDescriptor,
                                                 IGF.IGM.IntPtrTy);
   auto offset = IGF.Builder.CreateSub(reqtAddress, baseAddress);

   // Determine how to adjust the byte offset we have to make it a witness
   // table offset.
   const auto &dataLayout = IGF.IGM.Module.getDataLayout();
   auto protoReqSize =
      dataLayout.getTypeAllocSizeInBits(IGF.IGM.InterfaceRequirementStructTy);
   auto ptrSize = dataLayout.getTypeAllocSizeInBits(IGF.IGM.Int8PtrTy);
   assert(protoReqSize >= ptrSize && "> 64-bit pointers?");
   assert((protoReqSize % ptrSize == 0) && "Must be evenly divisible");
   (void)ptrSize;
   unsigned factor = protoReqSize / 8;
   auto factorConstant = llvm::ConstantInt::get(IGF.IGM.IntPtrTy, factor);
   return IGF.Builder.CreateUDiv(offset, factorConstant);
}

MetadataResponse
irgen::emitAssociatedTypeMetadataRef(IRGenFunction &IGF,
                                     llvm::Value *parentMetadata,
                                     llvm::Value *wtable,
                                     AssociatedType associatedType,
                                     DynamicMetadataRequest request) {
   auto &IGM = IGF.IGM;

   // Extract the requirements base descriptor.
   auto reqBaseDescriptor =
      IGM.getAddrOfInterfaceRequirementsBaseDescriptor(
         associatedType.getSourceInterface());

   // Extract the associated type descriptor.
   auto assocTypeDescriptor =
      IGM.getAddrOfAssociatedTypeDescriptor(associatedType.getAssociation());

   // Call swift_getAssociatedTypeWitness().
   auto call = IGF.Builder.CreateCall(IGM.getGetAssociatedTypeWitnessFn(),
                                      { request.get(IGF),
                                        wtable,
                                        parentMetadata,
                                        reqBaseDescriptor,
                                        assocTypeDescriptor });
   call->setDoesNotThrow();
   call->setDoesNotAccessMemory();
   return MetadataResponse::handle(IGF, request, call);
}

Signature
IRGenModule::getAssociatedTypeWitnessTableAccessFunctionSignature() {
   auto &fnType = AssociatedTypeWitnessTableAccessFunctionTy;
   if (!fnType) {
      // The associated type metadata is passed first so that this function is
      // CC-compatible with a conformance's witness table access function.
      fnType = llvm::FunctionType::get(WitnessTablePtrTy,
                                       { TypeMetadataPtrTy,
                                         TypeMetadataPtrTy,
                                         WitnessTablePtrTy },
         /*varargs*/ false);
   }

   auto attrs = llvm::AttributeList::get(getLLVMContext(),
                                         llvm::AttributeList::FunctionIndex,
                                         llvm::Attribute::NoUnwind);

   return Signature(fnType, attrs, SwiftCC);
}

/// Load a reference to the protocol descriptor for the given protocol.
///
/// For Swift protocols, this is a constant reference to the protocol descriptor
/// symbol.
/// For ObjC protocols, descriptors are uniqued at runtime by the ObjC runtime.
/// We need to load the unique reference from a global variable fixed up at
/// startup.
///
/// The result is always a InterfaceDescriptorRefTy whose low bit will be
/// set to indicate when this is an Objective-C protocol.
llvm::Value *irgen::emitInterfaceDescriptorRef(IRGenFunction &IGF,
                                               InterfaceDecl *protocol) {
   // @todo
   return IGF.Builder.CreatePtrToInt(
      IGF.IGM.getAddrOfInterfaceDescriptor(protocol),
      IGF.IGM.InterfaceDescriptorRefTy);
//   if (!protocol->isObjC()) {
//      return IGF.Builder.CreatePtrToInt(
//         IGF.IGM.getAddrOfInterfaceDescriptor(protocol),
//         IGF.IGM.InterfaceDescriptorRefTy);
//   }
//
//   llvm::Value *val = emitReferenceToObjCInterface(IGF, protocol);
//   val = IGF.Builder.CreatePtrToInt(val, IGF.IGM.InterfaceDescriptorRefTy);
//
//   // Set the low bit to indicate that this is an Objective-C protocol.
//   auto *isObjCBit = llvm::ConstantInt::get(IGF.IGM.InterfaceDescriptorRefTy, 1);
//   val = IGF.Builder.CreateOr(val, isObjCBit);
//
//   return val;
}

llvm::Constant *IRGenModule::getAddrOfGenericEnvironment(
   CanGenericSignature signature) {
   if (!signature)
      return nullptr;

   IRGenMangler mangler;
   auto symbolName = mangler.mangleSymbolNameForGenericEnvironment(signature);
   return getAddrOfStringForMetadataRef(
      symbolName, /*alignment=*/0, false,
      [&] (ConstantInitBuilder &builder) -> ConstantInitFuture {
         /// Collect the cumulative count of parameters at each level.
         llvm::SmallVector<uint16_t, 4> genericParamCounts;
         unsigned curDepth = 0;
         unsigned genericParamCount = 0;
         for (const auto gp : signature->getGenericParams()) {
            if (curDepth != gp->getDepth()) {
               genericParamCounts.push_back(genericParamCount);
               curDepth = gp->getDepth();
            }

            ++genericParamCount;
         }
         genericParamCounts.push_back(genericParamCount);

         auto flags = GenericEnvironmentFlags()
            .withNumGenericParameterLevels(genericParamCounts.size())
            .withNumGenericRequirements(signature->getRequirements().size());

         ConstantStructBuilder fields = builder.beginStruct();
         fields.setPacked(true);

         // Flags
         fields.addInt32(flags.getIntValue());

         // Parameter counts.
         for (auto count : genericParamCounts) {
            fields.addInt16(count);
         }

         // Generic parameters.
         signature->forEachParam([&](GenericTypeParamType *param,
                                     bool canonical) {
            fields.addInt(Int8Ty,
                          GenericParamDescriptor(GenericParamKind::Type,
                                                 canonical,
                                                 false)
                             .getIntValue());
         });

         // Generic requirements
         irgen::addGenericRequirements(*this, fields, signature,
                                       signature->getRequirements());
         return fields.finishAndCreateFuture();
      });
}