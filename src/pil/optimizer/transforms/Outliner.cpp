//===------------- Outliner.cpp - Outlining Transformations ---------------===//
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

#define DEBUG_TYPE "pil-outliner"

#include "polarphp/ast/AstMangler.h"
#include "polarphp/ast/Module.h"
#include "polarphp/ast/InterfaceConformance.h"
#include "polarphp/ast/Types.h"
#include "polarphp/demangling/Demangler.h"
#include "polarphp/pil/lang/DebugUtils.h"
#include "polarphp/pil/lang/PILBuilder.h"
#include "polarphp/pil/lang/PILFunction.h"
#include "polarphp/pil/optimizer/Utils/PILOptFunctionBuilder.h"
#include "polarphp/pil/lang/PILInstruction.h"
#include "polarphp/pil/lang/PILModule.h"
#include "polarphp/pil/optimizer/passmgr/Transforms.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace polar;

llvm::cl::opt<std::string> DumpFuncsBeforeOutliner(
   "sil-dump-functions-before-outliner", llvm::cl::init(""),
   llvm::cl::desc(
      "Break before running each function pass on a particular function"));

namespace {

class OutlinerMangler : public mangle::AstMangler {
   /// The kind of method bridged.
   enum MethodKind : unsigned {
      BridgedProperty,
      BridgedPropertyAddress,
      BridgedMethod,
   };

   llvm::BitVector *IsParameterBridged;
   PILDeclRef MethodDecl;
   MethodKind Kind;
   bool IsReturnBridged;

public:
   /// Create an mangler for an outlined bridged method.
   OutlinerMangler(PILDeclRef Method, llvm::BitVector *ParameterBridged,
                   bool ReturnBridged)
      : IsParameterBridged(ParameterBridged), MethodDecl(Method),
        Kind(BridgedMethod), IsReturnBridged(ReturnBridged) {}

   /// Create an mangler for an outlined bridged property.
   OutlinerMangler(PILDeclRef Method, bool IsAddress)
      : IsParameterBridged(nullptr), MethodDecl(Method),
        Kind(IsAddress ? BridgedPropertyAddress : BridgedProperty),
        IsReturnBridged(true) {}

   std::string mangle();

private:
   char getMethodKindMangling() {
      switch (Kind) {
         case BridgedProperty:
            return 'p';
         case BridgedPropertyAddress:
            return 'a';
         case BridgedMethod:
            return 'm';
      }
      llvm_unreachable("unhandled kind");
   }
};
} // end anonymous namespace.

std::string OutlinerMangler::mangle() {
   beginManglingWithoutPrefix();

   appendOperator(MethodDecl.mangle());

   llvm::SmallString<128> Buffer;
   llvm::raw_svector_ostream Out(Buffer);

   Out << getMethodKindMangling();
   if (IsParameterBridged)
      for (unsigned Idx = 0,  E = IsParameterBridged->size(); Idx != E; ++Idx)
         Out << (IsParameterBridged->test(Idx) ? 'b' : 'n');
   Out << (IsReturnBridged ? 'b' : 'n');
   Out << '_';

   appendOperator("Te", Buffer);
   return finalize();
}

namespace {

class OutlinePattern {
protected:
   PILOptFunctionBuilder &FuncBuilder;

public:
   OutlinePattern(PILOptFunctionBuilder &FuncBuilder) : FuncBuilder(FuncBuilder) {}

   /// Match the instruction sequence.
   virtual bool matchInstSequence(PILBasicBlock::iterator I) = 0;

   /// Outline the matched instruction sequence.
   ///
   /// If a new outlined function is created return the function. If the outlined
   /// function already existed return null.
   /// Returns the last instruction of the matched sequence after the
   /// replacement.
   virtual std::pair<PILFunction *, PILBasicBlock::iterator>
   outline(PILModule &M) = 0;

   virtual std::string getOutlinedFunctionName() = 0;

   virtual ~OutlinePattern() {}
};

/// Get the bridgeToObjectiveC witness for the type.
static PILDeclRef getBridgeToObjectiveC(CanType NativeType,
                                        ModuleDecl *SwiftModule) {
   auto &Ctx = SwiftModule->getAstContext();
   auto Proto = Ctx.getInterface(KnownInterfaceKind::ObjectiveCBridgeable);
   if (!Proto)
      return PILDeclRef();
   auto ConformanceRef =
      SwiftModule->lookupConformance(NativeType, Proto);
   if (ConformanceRef.isInvalid())
      return PILDeclRef();

   auto Conformance = ConformanceRef.getConcrete();
   // bridgeToObjectiveC
   DeclName Name(Ctx, Ctx.Id_bridgeToObjectiveC, llvm::ArrayRef<Identifier>());
   auto *Requirement = dyn_cast_or_null<FuncDecl>(
      Proto->getSingleRequirement(Name));
   if (!Requirement)
      return PILDeclRef();

   auto Witness = Conformance->getWitnessDecl(Requirement);
   return PILDeclRef(Witness);
}

/// Get the _unconditionallyBridgeFromObjectiveC witness for the type.
PILDeclRef getBridgeFromObjectiveC(CanType NativeType,
                                   ModuleDecl *SwiftModule) {
   auto &Ctx = SwiftModule->getAstContext();
   auto Proto = Ctx.getInterface(KnownInterfaceKind::ObjectiveCBridgeable);
   if (!Proto)
      return PILDeclRef();
   auto ConformanceRef =
      SwiftModule->lookupConformance(NativeType, Proto);
   if (ConformanceRef.isInvalid())
      return PILDeclRef();
   auto Conformance = ConformanceRef.getConcrete();
   // _unconditionallyBridgeFromObjectiveC
   DeclName Name(Ctx, Ctx.getIdentifier("_unconditionallyBridgeFromObjectiveC"),
                 llvm::makeArrayRef(Identifier()));
   auto *Requirement = dyn_cast_or_null<FuncDecl>(
      Proto->getSingleRequirement(Name));
   if (!Requirement)
      return PILDeclRef();

   auto Witness = Conformance->getWitnessDecl(Requirement);
   return PILDeclRef(Witness);
}

struct SwitchInfo {
   SwitchEnumInst *SwitchEnum = nullptr;
   PILBasicBlock *SomeBB = nullptr;
   PILBasicBlock *NoneBB = nullptr;
   BranchInst *Br = nullptr;
};

/// Pattern for a bridged property call.
///
///  bb7:
///    %30 = unchecked_take_enum_data_addr %19 : $*Optional<UITextField>, #Optional.some!enumelt.1
///    %31 = load %30 : $*UITextField
///    strong_retain %31 : $UITextField
///    %33 = objc_method %31 : $UITextField, #UITextField.text!getter.1.foreign : (UITextField) -> () -> String?, $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
///    %34 = apply %33(%31) : $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
///    switch_enum %34 : $Optional<NSString>, case #Optional.some!enumelt.1: bb8, case #Optional.none!enumelt: bb9
///
///  bb8(%36 : $NSString):
///    // function_ref static String._unconditionallyBridgeFromObjectiveC(_:)
///    %37 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveCSSSo8NSStringCSgFZ : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
///    %38 = enum $Optional<NSString>, #Optional.some!enumelt.1, %36 : $NSString
///    %39 = metatype $@thin String.Type
///    %40 = apply %37(%38, %39) : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
///    %41 = enum $Optional<String>, #Optional.some!enumelt.1, %40 : $String
///    br bb10(%41 : $Optional<String>)
///
///  bb9:
///    %43 = enum $Optional<String>, #Optional.none!enumelt
///    br bb10(%43 : $Optional<String>)
///
///  bb10(%45 : $Optional<String>):
class BridgedProperty : public OutlinePattern {
   std::string OutlinedName;
   SingleValueInstruction *FirstInst; // A load or class_method
   PILBasicBlock *StartBB;
   SwitchInfo switchInfo;
   ObjCMethodInst *ObjCMethod;
   StrongReleaseInst *Release;
   ApplyInst *PropApply;

public:
   bool matchInstSequence(PILBasicBlock::iterator I) override;

   std::pair<PILFunction *, PILBasicBlock::iterator>
   outline(PILModule &M) override;

   BridgedProperty(PILOptFunctionBuilder &FuncBuilder) : OutlinePattern(FuncBuilder) {
      clearState();
   }

   BridgedProperty(const BridgedProperty&) = delete;
   BridgedProperty& operator=(const BridgedProperty&) = delete;

   virtual ~BridgedProperty() {}

   std::string getOutlinedFunctionName() override;

private:
   bool matchMethodCall(PILBasicBlock::iterator);
   CanPILFunctionType getOutlinedFunctionType(PILModule &M);
   void clearState();
};
}

void BridgedProperty::clearState() {
   FirstInst = nullptr;
   StartBB = nullptr;
   switchInfo = SwitchInfo();
   ObjCMethod = nullptr;
   Release = nullptr;
   PropApply = nullptr;
   OutlinedName.clear();
}

std::string BridgedProperty::getOutlinedFunctionName() {
   if (OutlinedName.empty()) {
      OutlinerMangler Mangler(ObjCMethod->getMember(), isa<LoadInst>(FirstInst));
      OutlinedName = Mangler.mangle();
   }
   return OutlinedName;
}

/// Returns the outlined function type.
///
/// This depends on the first instruction we matched. Either we matched a load
/// or we started the match at the class method instruction.
///
/// load %30 : *UITextField:
///   (@in_guaranteed InstanceType) -> (@owned Optional<BridgedInstanceType>)
/// objc_method %31 : UITextField
///   (@unowned InstanceType) -> (@owned Optional<BridgedInstanceType>)
///
CanPILFunctionType BridgedProperty::getOutlinedFunctionType(PILModule &M) {
   SmallVector<PILParameterInfo, 4> Parameters;
   if (auto *Load = dyn_cast<LoadInst>(FirstInst))
      Parameters.push_back(
         PILParameterInfo(Load->getType().getAstType(),
                          ParameterConvention::Indirect_In_Guaranteed));
   else
      Parameters.push_back(PILParameterInfo(cast<ObjCMethodInst>(FirstInst)
                                               ->getOperand()
                                               ->getType()
                                               .getAstType(),
                                            ParameterConvention::Direct_Unowned));
   SmallVector<PILResultInfo, 4> Results;

   Results.push_back(PILResultInfo(
      switchInfo.Br->getArg(0)->getType().getAstType(),
      ResultConvention::Owned));
   auto ExtInfo =
      PILFunctionType::ExtInfo(PILFunctionType::Representation::Thin,
         /*pseudogeneric*/ false, /*noescape*/ false,
                               DifferentiabilityKind::NonDifferentiable,
         /*clangFunctionType*/ nullptr);
   auto FunctionType = PILFunctionType::get(
      nullptr, ExtInfo, PILCoroutineKind::None,
      ParameterConvention::Direct_Unowned, Parameters, /*yields*/ {},
      Results, None,
      SubstitutionMap(), false,
      M.getAstContext());
   return FunctionType;
}

std::pair<PILFunction *, PILBasicBlock::iterator>
BridgedProperty::outline(PILModule &M) {
   // Get the function type.
   auto FunctionType = getOutlinedFunctionType(M);

   std::string nameTmp = getOutlinedFunctionName();
   auto name = M.allocateCopy(nameTmp);

   auto *Fun = FuncBuilder.getOrCreateFunction(
      ObjCMethod->getLoc(), name, PILLinkage::Shared, FunctionType, IsNotBare,
      IsNotTransparent, IsSerializable, IsNotDynamic);
   bool NeedsDefinition = Fun->empty();

   if (Release) {
      // Move the release after the call.
      Release->moveBefore(StartBB->getTerminator());
   }

   //     [StartBB]
   //    /        \
  // [NoneBB]  [SomeBB]
   //   \          /
   //   [OldMergeBB]
   //
   //   Split to:
   //
   //      [StartBB]
   //          |
   //   [OutlinedEntryBB]   }
   //    /        \         }
   // [NoneBB]  [SomeBB]    } outlined
   //   \          /        }
   //   [OldMergeBB]        }
   //       |
   //   [NewTailBB]
   //
   auto *OutlinedEntryBB = StartBB->split(PILBasicBlock::iterator(FirstInst));
   auto *OldMergeBB = switchInfo.Br->getDestBB();
   auto *NewTailBB = OldMergeBB->split(OldMergeBB->begin());

   // Call the outlined function.
   {
      PILBuilder Builder(StartBB);
      auto Loc = FirstInst->getLoc();
      PILValue FunRef(Builder.createFunctionRef(Loc, Fun));
      PILValue Apply(
         Builder.createApply(Loc, FunRef, SubstitutionMap(),
                             {FirstInst->getOperand(0)}));
      Builder.createBranch(Loc, NewTailBB);
      OldMergeBB->getArgument(0)->replaceAllUsesWith(Apply);
   }

   if (!NeedsDefinition) {
      // Delete the outlined instructions/blocks.
      if (Release)
         Release->eraseFromParent();
      OutlinedEntryBB->eraseInstructions();
      OutlinedEntryBB->eraseFromParent();
      switchInfo.NoneBB->eraseInstructions();
      switchInfo.NoneBB->eraseFromParent();
      switchInfo.SomeBB->eraseInstructions();
      switchInfo.SomeBB->eraseFromParent();
      OldMergeBB->eraseInstructions();
      OldMergeBB->eraseFromParent();
      return std::make_pair(nullptr, std::prev(StartBB->end()));
   }

   if (!OutlinedEntryBB->getParent()->hasOwnership())
      Fun->setOwnershipEliminated();

   Fun->setInlineStrategy(NoInline);

   // Move the blocks into the new function.
   auto &FromBlockList = OutlinedEntryBB->getParent()->getBlocks();
   Fun->getBlocks().splice(Fun->begin(), FromBlockList, OldMergeBB);
   Fun->getBlocks().splice(Fun->begin(), FromBlockList, switchInfo.NoneBB);
   Fun->getBlocks().splice(Fun->begin(), FromBlockList, switchInfo.SomeBB);
   Fun->getBlocks().splice(Fun->begin(), FromBlockList, OutlinedEntryBB);

   // Create the function argument and return.
   auto *Load = dyn_cast<LoadInst>(FirstInst);
   PILBuilder Builder(FirstInst);
   if (Load) {
      OutlinedEntryBB->createFunctionArgument(Load->getOperand()->getType());
      auto *NewLoad =
         Builder.createLoad(Load->getLoc(), OutlinedEntryBB->getArgument(0),
                            Load->getOwnershipQualifier());
      Load->replaceAllUsesWith(NewLoad);
      Load->eraseFromParent();
   } else {
      OutlinedEntryBB->createFunctionArgument(
         FirstInst->getOperand(0)->getType());
      auto *Arg = OutlinedEntryBB->getArgument(0);
      FirstInst->setOperand(0, Arg);
      PropApply->setArgument(0, Arg);
   }
   Builder.setInsertionPoint(OldMergeBB);
   Builder.createReturn(ObjCMethod->getLoc(), OldMergeBB->getArgument(0));
   return std::make_pair(Fun, std::prev(StartBB->end()));
}

#define ADVANCE_ITERATOR_OR_RETURN_FALSE(It)                                   \
  do {                                                                         \
    if (It->getParent()->end() == ++It)                                        \
      return false;                                                            \
  } while (0);

static bool matchSwitch(SwitchInfo &SI, PILInstruction *Inst,
                        PILValue SwitchOperand) {
   auto *SwitchEnum = dyn_cast<SwitchEnumInst>(Inst);
   if (!SwitchEnum || SwitchEnum->getNumCases() != 2 ||
       SwitchEnum->getOperand() != SwitchOperand)
      return false;

   auto *SwitchBB = SwitchEnum->getParent();
   PILBasicBlock *SomeBB = SwitchEnum->getCase(0).second;
   PILBasicBlock *NoneBB = SwitchEnum->getCase(1).second;
   if (NoneBB->getSinglePredecessorBlock() != SwitchBB)
      return false;
   if (SomeBB->getSinglePredecessorBlock() != SwitchBB)
      return false;
   if (NoneBB->args_size() == 1)
      std::swap(NoneBB, SomeBB);
   if (SomeBB->args_size() != 1 || NoneBB->args_size() != 0)
      return false;

   // bb9:
   // %43 = enum $Optional<String>, #Optional.none!enumelt
   auto It = NoneBB->begin();
   auto *NoneEnum = dyn_cast<EnumInst>(It);
   if (!NoneEnum || NoneEnum->hasOperand() || !NoneEnum->hasOneUse())
      return false;

   // br bb10(%43 : $Optional<String>)
   ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
   auto *Br1 = dyn_cast<BranchInst>(It);
   if (!Br1 || Br1->getNumArgs() != 1 || Br1->getArg(0) != NoneEnum)
      return false;
   auto *MergeBB = Br1->getDestBB();

   // bb8(%36 : $NSString):
   It = SomeBB->begin();
   auto *SomeBBArg = SomeBB->getArgument(0);
   if (!SomeBBArg->hasOneUse())
      return false;

   // %37 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveCSSSo8NSStringCSgFZ : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
   auto *FunRef = dyn_cast<FunctionRefInst>(It);
   if (!FunRef || !FunRef->hasOneUse())
      return false;

   // %38 = enum $Optional<NSString>, #Optional.some!enumelt.1, %36 : $NSString
   ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
   auto *SomeEnum = dyn_cast<EnumInst>(It);
   if (!SomeEnum || !SomeEnum->hasOperand() || SomeEnum->getOperand() != SomeBBArg)
      return false;
   size_t numSomeEnumUses = std::distance(SomeEnum->use_begin(), SomeEnum->use_end());
   if (numSomeEnumUses > 2)
      return false;

   // %39 = metatype $@thin String.Type
   ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
   auto *Metatype = dyn_cast<MetatypeInst>(It);
   if (!Metatype || !Metatype->hasOneUse())
      return false;

   // %40 = apply %37(%38, %39) : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
   ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
   auto *Apply = dyn_cast<ApplyInst>(It);
   if (!Apply || !Apply->hasOneUse() || Apply->getCallee() != FunRef ||
       Apply->getNumArguments() != 2 || Apply->getArgument(0) != SomeEnum ||
       Apply->getArgument(1) != Metatype ||
       Apply->getSubstCalleeType()->getNumResults() != 1)
      return false;
   if (Apply->getSubstCalleeType()->getSingleResult().getConvention() !=
       ResultConvention::Owned)
      return false;

   // Check that we call the _unconditionallyBridgeFromObjectiveC witness.
   auto NativeType = Apply->getType().getAstType();
   auto *BridgeFun = FunRef->getInitiallyReferencedFunction();
   auto *SwiftModule = BridgeFun->getModule().getPolarphpModule();
   auto bridgeWitness = getBridgeFromObjectiveC(NativeType, SwiftModule);
   if (BridgeFun->getName() != bridgeWitness.mangle())
      return false;

   // %41 = enum $Optional<String>, #Optional.some!enumelt.1, %40 : $String
   ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
   auto *Enum3 = dyn_cast<EnumInst>(It);
   if (!Enum3 || !Enum3->hasOneUse() || !Enum3->hasOperand() ||
       Enum3->getOperand() != Apply)
      return false;

   if (numSomeEnumUses == 2) {
      // release_value %38 : $Optional<NSString>
      ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
      auto *RVI = dyn_cast<ReleaseValueInst>(It);
      if (!RVI || RVI->getOperand() != SomeEnum)
         return false;
   }

   // br bb10(%41 : $Optional<String>)
   ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
   auto *Br = dyn_cast<BranchInst>(It);
   if (!Br || Br->getDestBB() != MergeBB || Br->getNumArgs() != 1 ||
       Br->getArg(0) != Enum3)
      return false;

   SI.SwitchEnum = SwitchEnum;
   SI.SomeBB = SomeBB;
   SI.NoneBB = NoneBB;
   SI.Br = Br;
   return true;
}

bool BridgedProperty::matchMethodCall(PILBasicBlock::iterator It) {
   // Matches:
   //    %33 = objc_method %31 : $UITextField, #UITextField.text!getter.1.foreign : (UITextField) -> () -> String?, $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
   //    %34 = apply %33(%31) : $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
   //    switch_enum %34 : $Optional<NSString>, case #Optional.some!enumelt.1: bb8, case #Optional.none!enumelt: bb9
   //
   //  bb8(%36 : $NSString):
   //    %37 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveCSSSo8NSStringCSgFZ : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
   //    %38 = enum $Optional<NSString>, #Optional.some!enumelt.1, %36 : $NSString
   //    %39 = metatype $@thin String.Type
   //    %40 = apply %37(%38, %39) : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
   //    %41 = enum $Optional<String>, #Optional.some!enumelt.1, %40 : $String
   //    br bb10(%41 : $Optional<String>)
   //
   //  bb9:
   //    %43 = enum $Optional<String>, #Optional.none!enumelt
   //    br bb10(%43 : $Optional<String>)
   //
   //  bb10(%45 : $Optional<String>):
   //

   // %33 = objc_method %31 : $UITextField, #UITextField.text!getter.1.foreign
   ObjCMethod = dyn_cast<ObjCMethodInst>(It);
   PILValue Instance =
      FirstInst != ObjCMethod ? FirstInst : ObjCMethod->getOperand();
   if (!ObjCMethod || !ObjCMethod->hasOneUse() ||
       ObjCMethod->getOperand() != Instance ||
       ObjCMethod->getFunction()->getLoweredFunctionType()->isPolymorphic() ||
       ObjCMethod->getType().castTo<PILFunctionType>()->isPolymorphic() ||
       ObjCMethod->getType().castTo<PILFunctionType>()->hasOpenedExistential())
      return false;

   // Don't outline in the outlined function.
   if (ObjCMethod->getFunction()->getName().equals(getOutlinedFunctionName()))
      return false;

   // %34 = apply %33(%31) : $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
   ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
   PropApply = dyn_cast<ApplyInst>(It);
   if (!PropApply || PropApply->getCallee() != ObjCMethod ||
       PropApply->getNumArguments() != 1 ||
       PropApply->getArgument(0) != Instance || !PropApply->hasOneUse())
      return false;

   // switch_enum %34 : $Optional<NSString>, case #Optional.some!enumelt.1: bb8, case #Optional.none!enumelt: bb9
   ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
   return matchSwitch(switchInfo, &*It, PropApply);
}

bool BridgedProperty::matchInstSequence(PILBasicBlock::iterator It) {
   // Matches:
   // [ optionally:
   //    %31 = load %30 : $*UITextField
   //    strong_retain %31 : $UITextField
   // ]
   //    %33 = objc_method %31 : $UITextField, #UITextField.text!getter.1.foreign : (UITextField) -> () -> String?, $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
   //    %34 = apply %33(%31) : $@convention(objc_method) (UITextField) -> @autoreleased Optional<NSString>
   //    switch_enum %34 : $Optional<NSString>, case #Optional.some!enumelt.1: bb8, case #Optional.none!enumelt: bb9
   //
   //  bb8(%36 : $NSString):
   //    %37 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveCSSSo8NSStringCSgFZ : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
   //    %38 = enum $Optional<NSString>, #Optional.some!enumelt.1, %36 : $NSString
   //    %39 = metatype $@thin String.Type
   //    %40 = apply %37(%38, %39) : $@convention(method) (@owned Optional<NSString>, @thin String.Type) -> @owned String
   //    %41 = enum $Optional<String>, #Optional.some!enumelt.1, %40 : $String
   //    br bb10(%41 : $Optional<String>)
   //
   //  bb9:
   //    %43 = enum $Optional<String>, #Optional.none!enumelt
   //    br bb10(%43 : $Optional<String>)
   //
   //  bb10(%45 : $Optional<String>):

   clearState();

   // %31 = load %30 : $*UITextField
   auto *Load = dyn_cast<LoadInst>(It);

   // Otherwise, trying matching from the method call.
   if (!Load) {
      // Try to match without the load/strong_retain prefix.
      auto *CMI = dyn_cast<ObjCMethodInst>(It);
      if (!CMI || CMI->getFunction()->getLoweredFunctionType()->isPolymorphic() ||
          CMI->getType().castTo<PILFunctionType>()->isPolymorphic() ||
          CMI->getType().castTo<PILFunctionType>()->hasOpenedExistential())
         return false;
      FirstInst = CMI;
   } else
      FirstInst = Load;

   StartBB = FirstInst->getParent();

   if (Load) {
      // strong_retain %31 : $UITextField
      ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
      auto *Retain = dyn_cast<StrongRetainInst>(It);
      if (!Retain || Retain->getOperand() != Load)
         return false;
      ADVANCE_ITERATOR_OR_RETURN_FALSE(It);
   }

   if (!matchMethodCall(It))
      return false;

   if (Load) {
      // There will be a release matching the earlier retain. The only user of the
      // retained value is the unowned objective-c method consumer.
      unsigned NumUses = 0;
      Release = nullptr;
      for (auto *Use : Load->getUses()) {
         ++NumUses;
         if (auto *R = dyn_cast<StrongReleaseInst>(Use->getUser())) {
            if (!Release) {
               Release = R;
            } else {
               Release = nullptr;
               break;
            }
         }
      }
      if (!Release || NumUses != 4)
         return false;
   }
   return true;
}


namespace {
/// Match a bridged argument.
/// %15 = function_ref @$SSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF
/// %16 = apply %15(%14) :
///         $@convention(method) (@guaranteed String) -> @owned NSString
/// %17 = enum $Optional<NSString>, #Optional.some!enumelt.1, %16 : $NSString
/// release_value %14 : $String
///
/// apply %objcMethod(%17, ...) : $@convention(objc_method) (Optional<NSString> ...) ->
/// release_value %17 : $Optional<NSString>
class BridgedArgument {
public:
   FunctionRefInst *BridgeFun;
   ApplyInst *BridgeCall;
   EnumInst *OptionalResult;
   ReleaseValueInst *ReleaseAfterBridge;
   ReleaseValueInst *ReleaseArgAfterCall;
   unsigned Idx = 0;

   // Matched bridged argument.
   BridgedArgument(unsigned Idx, FunctionRefInst *F, ApplyInst *A, EnumInst *E,
                   ReleaseValueInst *R0, ReleaseValueInst *R1)
      : BridgeFun(F), BridgeCall(A), OptionalResult(E), ReleaseAfterBridge(R0),
        ReleaseArgAfterCall(R1), Idx(Idx) {}

   /// Invalid argument constructor.
   BridgedArgument()
      : BridgeFun(nullptr), BridgeCall(nullptr), OptionalResult(nullptr),
        ReleaseAfterBridge(nullptr), ReleaseArgAfterCall(nullptr), Idx(0) {}

   static BridgedArgument match(unsigned ArgIdx, PILValue Arg, ApplyInst *AI);

   operator bool() const { return BridgeFun != nullptr; }
   PILValue bridgedValue() { return ReleaseAfterBridge->getOperand(); }

   void eraseFromParent();

   /// Move the bridged argument sequence to the bridged call block.
   /// Precondition: The bridged call has already been moved to the outlined
   /// function.
   void transferTo(PILValue BridgedValueFunArg, ApplyInst *BridgedCall);
};
}

void BridgedArgument::transferTo(PILValue BridgedValue,
                                 ApplyInst *BridgedCall) {
   assert(BridgedCall->getParent() != BridgeFun->getParent());
   // Move the instructions to the bridged call that we have already moved and
   // update the uses of the bridge value by the function argument value passed
   // to this function.
   auto *DestBB = BridgedCall->getParent();
   DestBB->moveTo(PILBasicBlock::iterator(BridgedCall), BridgeFun);
   DestBB->moveTo(PILBasicBlock::iterator(BridgedCall), BridgeCall);
   BridgeCall->setArgument(0, BridgedValue);
   DestBB->moveTo(PILBasicBlock::iterator(BridgedCall), OptionalResult);
   DestBB->moveTo(PILBasicBlock::iterator(BridgedCall), ReleaseAfterBridge);
   ReleaseAfterBridge->setOperand(BridgedValue);
   auto AfterCall = std::next(PILBasicBlock::iterator(BridgedCall));
   DestBB->moveTo(PILBasicBlock::iterator(AfterCall), ReleaseArgAfterCall);
}

void BridgedArgument::eraseFromParent() {
   ReleaseAfterBridge->eraseFromParent();
   ReleaseArgAfterCall->eraseFromParent();
   OptionalResult->eraseFromParent();
   BridgeCall->eraseFromParent();
   BridgeFun->eraseFromParent();
}

BridgedArgument BridgedArgument::match(unsigned ArgIdx, PILValue Arg,
                                       ApplyInst *AI) {
   // Match
   // %15 = function_ref @$SSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF
   // %16 = apply %15(%14) :
   //         $@convention(method) (@guaranteed String) -> @owned NSString
   // %17 = enum $Optional<NSString>, #Optional.some!enumelt.1, %16 : $NSString
   // release_value %14 : $String
   // ...
   // apply %objcMethod(%17, ...) : $@convention(objc_method) (Optional<NSString> ...) ->
   // release_value ...
   // release_value %17 : $Optional<NSString>
   //
   auto *Enum = dyn_cast<EnumInst>(Arg);
   if (!Enum || !Enum->hasOperand())
      return BridgedArgument();

   if (PILBasicBlock::iterator(Enum) == Enum->getParent()->begin())
      return BridgedArgument();
   auto *BridgeCall =
      dyn_cast<ApplyInst>(std::prev(PILBasicBlock::iterator(Enum)));
   if (!BridgeCall || BridgeCall->getNumArguments() != 1 ||
       Enum->getOperand() != BridgeCall || !BridgeCall->hasOneUse())
      return BridgedArgument();

   auto BridgedValue = BridgeCall->getArgument(0);
   auto Next = std::next(PILBasicBlock::iterator(Enum));
   if (Next == Enum->getParent()->end())
      return BridgedArgument();
   auto *BridgedValueRelease =
      dyn_cast<ReleaseValueInst>(std::next(PILBasicBlock::iterator(Enum)));
   if (!BridgedValueRelease || BridgedValueRelease->getOperand() != BridgedValue)
      return BridgedArgument();

   if (PILBasicBlock::iterator(BridgeCall) == BridgeCall->getParent()->begin())
      return BridgedArgument();
   auto *FunRef =
      dyn_cast<FunctionRefInst>(std::prev(PILBasicBlock::iterator(BridgeCall)));
   if (!FunRef || !FunRef->hasOneUse() || BridgeCall->getCallee() != FunRef)
      return BridgedArgument();

   ReleaseValueInst *ReleaseAfter = nullptr;
   for (auto *Use : Enum->getUses()) {
      if (Use->getUser() == AI)
         continue;

      // The enum must only have two uses the release and the apply.
      if (ReleaseAfter)
         return BridgedArgument();

      ReleaseAfter = dyn_cast<ReleaseValueInst>(Use->getUser());
      if (!ReleaseAfter)
         return BridgedArgument();
   }

   // Make sure we are calling the actual bridge witness.
   auto NativeType = BridgedValue->getType().getAstType();
   auto *BridgeFun = FunRef->getInitiallyReferencedFunction();
   auto *SwiftModule = BridgeFun->getModule().getPolarphpModule();
   auto bridgeWitness = getBridgeToObjectiveC(NativeType, SwiftModule);
   if (BridgeFun->getName() != bridgeWitness.mangle())
      return BridgedArgument();

   return BridgedArgument(ArgIdx, FunRef, BridgeCall, Enum, BridgedValueRelease,
                          ReleaseAfter);
}

namespace {
// Match the return value briding pattern.
//   switch_enum %20 : $Optional<NSString>, case #O.some: bb1, case #O.none: bb2
//
// bb1(%23 : $NSString):
//   %24 = function_ref @_unconditionallyBridgeFromObjectiveC
//   %25 = enum $Optional<NSString>, #Optional.some!enumelt.1, %23 : $NSString
//   %26 = metatype $@thin String.Type
//   %27 = apply %24(%25, %26)
//   %28 = enum $Optional<String>, #Optional.some!enumelt.1, %27 : $String
//   br bb3(%28 : $Optional<String>)
//
// bb2:
//   %30 = enum $Optional<String>, #Optional.none!enumelt
//   br bb3(%30 : $Optional<String>)
//
// bb3(%32 : $Optional<String>):
class BridgedReturn {
   SwitchInfo switchInfo;
public:
   bool match(ApplyInst *BridgedCall) {
      switchInfo = SwitchInfo();
      auto *SwitchBB = BridgedCall->getParent();
      return matchSwitch(switchInfo, SwitchBB->getTerminator(), BridgedCall);
   }

   operator bool() { return switchInfo.SomeBB != nullptr; }

   CanType getReturnType() {
      return switchInfo.Br->getArg(0)->getType().getAstType();
   }

   /// Outline the return value bridging blocks.
   void outline(PILFunction *Fun, ApplyInst *NewOutlinedCall);
};
}

void BridgedReturn::outline(PILFunction *Fun, ApplyInst *NewOutlinedCall) {
// Outline the bridged return result blocks.
//   switch_enum %20 : $Optional<NSString>, case #O.some: bb1, case #O.none: bb2
//
// bb1(%23 : $NSString):
//   %24 = function_ref @$SSS10FoundationE36_unconditionallyBridgeFromObjectiveC
//   %25 = enum $Optional<NSString>, #Optional.some!enumelt.1, %23 : $NSString
//   %26 = metatype $@thin String.Type
//   %27 = apply %24(%25, %26)
//   %28 = enum $Optional<String>, #Optional.some!enumelt.1, %27 : $String
//   br bb3(%28 : $Optional<String>)
//
// bb2:
//   %30 = enum $Optional<String>, #Optional.none!enumelt
//   br bb3(%30 : $Optional<String>)
//
// bb3(%32 : $Optional<String>):

   auto *StartBB = switchInfo.SwitchEnum->getParent();
   auto *OutlinedEntryBB = StartBB->split(PILBasicBlock::iterator(switchInfo.SwitchEnum));
   auto *OldMergeBB = switchInfo.Br->getDestBB();
   auto *NewTailBB = OldMergeBB->split(OldMergeBB->begin());
   auto Loc = switchInfo.SwitchEnum->getLoc();

   {
      PILBuilder Builder(StartBB);
      Builder.createBranch(Loc, NewTailBB);
      OldMergeBB->getArgument(0)->replaceAllUsesWith(NewOutlinedCall);
   }

   // Outlined function already existed. Just delete instructions and wire up
   // blocks.
   if (!Fun) {
      OutlinedEntryBB->eraseInstructions();
      OutlinedEntryBB->eraseFromParent();
      switchInfo.NoneBB->eraseInstructions();
      switchInfo.NoneBB->eraseFromParent();
      switchInfo.SomeBB->eraseInstructions();
      switchInfo.SomeBB->eraseFromParent();
      OldMergeBB->eraseInstructions();
      OldMergeBB->eraseFromParent();
      return;
   }

   // Move the blocks into the new function.
   assert(Fun->begin() != Fun->end() &&
          "The entry block must already have been created");
   PILBasicBlock *EntryBB = &*Fun->begin();
   auto &FromBlockList = OutlinedEntryBB->getParent()->getBlocks();
   Fun->getBlocks().splice(Fun->begin(), FromBlockList, OldMergeBB);
   OldMergeBB->moveAfter(EntryBB);
   auto InsertPt = PILFunction::iterator(OldMergeBB);
   Fun->getBlocks().splice(InsertPt, FromBlockList, OutlinedEntryBB);
   Fun->getBlocks().splice(InsertPt, FromBlockList, switchInfo.NoneBB);
   Fun->getBlocks().splice(InsertPt, FromBlockList, switchInfo.SomeBB);

   PILBuilder Builder (EntryBB);
   Builder.createBranch(Loc, OutlinedEntryBB);

   Builder.setInsertionPoint(OldMergeBB);
   Builder.createReturn(Loc, OldMergeBB->getArgument(0));
}

// @todo
//namespace {
//class ObjCMethodCall : public OutlinePattern {
//   ObjCMethodInst *ObjCMethod;
//   ApplyInst *BridgedCall;
//   SmallVector<BridgedArgument, 4> BridgedArguments;
//   std::string OutlinedName;
//   llvm::BitVector IsBridgedArgument;
//   BridgedReturn BridgedReturn;
//public:
//   bool matchInstSequence(PILBasicBlock::iterator I) override;
//
//   std::pair<PILFunction *, PILBasicBlock::iterator>
//   outline(PILModule &M) override;
//
//   ObjCMethodCall(PILOptFunctionBuilder &FuncBuilder)
//      : OutlinePattern(FuncBuilder) {}
//   ~ObjCMethodCall();
//
//private:
//   void clearState();
//   std::string getOutlinedFunctionName() override;
//   CanPILFunctionType getOutlinedFunctionType(PILModule &M);
//};
//}

//ObjCMethodCall::~ObjCMethodCall() {
//   clearState();
//}
//
//void ObjCMethodCall::clearState() {
//   ObjCMethod = nullptr;
//   BridgedCall = nullptr;
//   BridgedArguments.clear();
//   OutlinedName.clear();
//   IsBridgedArgument.clear();
//}
//
//std::pair<PILFunction *, PILBasicBlock::iterator>
//ObjCMethodCall::outline(PILModule &M) {
//
//   auto FunctionType = getOutlinedFunctionType(M);
//   std::string nameTmp = getOutlinedFunctionName();
//   auto name = M.allocateCopy(nameTmp);
//
//   auto *Fun = FuncBuilder.getOrCreateFunction(
//      ObjCMethod->getLoc(), name, PILLinkage::Shared, FunctionType, IsNotBare,
//      IsNotTransparent, IsSerializable, IsNotDynamic);
//   bool NeedsDefinition = Fun->empty();
//
//   // Call the outlined function.
//   ApplyInst *OutlinedCall;
//   {
//      PILBuilder Builder(BridgedCall);
//
//      auto Loc = BridgedCall->getLoc();
//      PILValue FunRef(Builder.createFunctionRef(Loc, Fun));
//
//      // Collect the arguments for the apply.
//      SmallVector<PILValue, 8> Args;
//      unsigned OrigSigIdx = 0;
//      unsigned BridgedArgIdx = 0;
//      for (auto Arg : BridgedCall->getArguments()) {
//         if (BridgedArgIdx < BridgedArguments.size() &&
//             BridgedArguments[BridgedArgIdx].Idx == OrigSigIdx) {
//            Args.push_back(BridgedArguments[BridgedArgIdx].bridgedValue());
//            ++BridgedArgIdx;
//         } else {
//            // Otherwise, use the original type convention.
//            Args.push_back(Arg);
//         }
//         OrigSigIdx++;
//      }
//      OutlinedCall = Builder.createApply(Loc, FunRef, SubstitutionMap(), Args);
//      if (!BridgedCall->use_empty() && !BridgedReturn)
//         BridgedCall->replaceAllUsesWith(OutlinedCall);
//   }
//
//   // Outlined function already exists. Only need to delete basic blocks/instructions.
//   if (!NeedsDefinition) {
//      if (BridgedReturn)
//         BridgedReturn.outline(nullptr, OutlinedCall);
//      BridgedCall->eraseFromParent();
//      ObjCMethod->eraseFromParent();
//      // Remove bridged argument code.
//      for (auto Arg : BridgedArguments)
//         Arg.eraseFromParent();
//      PILBasicBlock::iterator I(OutlinedCall);
//      return std::make_pair(Fun, I);
//   }
//
//   if (!ObjCMethod->getFunction()->hasOwnership())
//      Fun->setOwnershipEliminated();
//
//   Fun->setInlineStrategy(NoInline);
//
//   // Create the entry block.
//   auto *EntryBB = Fun->createBasicBlock();
//
//   // Move the bridged call.
//   EntryBB->moveTo(EntryBB->end(), ObjCMethod);
//   EntryBB->moveTo(EntryBB->end(), BridgedCall);
//
//   // Create the arguments.
//   unsigned OrigSigIdx = 0;
//   unsigned BridgedArgIdx = 0;
//   PILValue LastArg;
//   for (auto Arg : BridgedCall->getArguments()) {
//      if (BridgedArgIdx < BridgedArguments.size() &&
//          BridgedArguments[BridgedArgIdx].Idx == OrigSigIdx) {
//         auto &BridgedArg = BridgedArguments[BridgedArgIdx];
//         auto *FunArg =
//            EntryBB->createFunctionArgument(BridgedArg.bridgedValue()->getType());
//         BridgedArg.transferTo(FunArg, BridgedCall);
//         ++BridgedArgIdx;
//      } else {
//         auto *FunArg = EntryBB->createFunctionArgument(Arg->getType());
//         BridgedCall->setArgument(OrigSigIdx, FunArg);
//         LastArg = FunArg;
//      }
//      OrigSigIdx++;
//   }
//
//   // Set the method lookup's target.
//   ObjCMethod->setOperand(LastArg);
//
//   // Create the return and optionally move the bridging code.
//   if (!BridgedReturn) {
//      PILBuilder Builder(EntryBB);
//      Builder.createReturn(BridgedCall->getLoc(), BridgedCall);
//   } else {
//      BridgedReturn.outline(Fun, OutlinedCall);
//   }
//
//   PILBasicBlock::iterator I(OutlinedCall);
//   return std::make_pair(Fun, I);
//}
//
//std::string ObjCMethodCall::getOutlinedFunctionName() {
//   if (OutlinedName.empty()) {
//      OutlinerMangler Mangler(ObjCMethod->getMember(), &IsBridgedArgument,
//                              BridgedReturn);
//      OutlinedName = Mangler.mangle();
//   }
//   return OutlinedName;
//}
//
//bool ObjCMethodCall::matchInstSequence(PILBasicBlock::iterator I) {
//   clearState();
//
//   ObjCMethod = dyn_cast<ObjCMethodInst>(I);
//   if (!ObjCMethod ||
//       ObjCMethod->getFunction()->getLoweredFunctionType()->isPolymorphic() ||
//       ObjCMethod->getType().castTo<PILFunctionType>()->isPolymorphic() ||
//       ObjCMethod->getType().castTo<PILFunctionType>()->hasOpenedExistential())
//      return false;
//
//   auto *Use = ObjCMethod->getSingleUse();
//   if (!Use)
//      return false;
//   BridgedCall = dyn_cast<ApplyInst>(Use->getUser());
//   if (!BridgedCall ||
//       (!BridgedCall->hasOneUse() && !BridgedCall->use_empty()) ||
//       ObjCMethod->getParent() != BridgedCall->getParent())
//      return false;
//
//   // Collect bridged parameters.
//   unsigned Idx = 0;
//   IsBridgedArgument.resize(BridgedCall->getNumArguments(), false);
//   for (auto &Param : BridgedCall->getArgumentOperands()) {
//      unsigned CurIdx = Idx++;
//
//      // Look for Optional<NSFoo> type.
//      auto Ty = Param.get()->getType().getOptionalObjectType();
//      if (!Ty)
//         continue;
//
//      // Can't handle AnyObject. The concrete class type can be different since we
//      // are passing 'id'. To make this work we would have to mangle the type into
//      // the function name.
//      if (Ty.isAnyObject())
//         continue;
//
//      auto BridgedArg = BridgedArgument::match(CurIdx, Param.get(), BridgedCall);
//      if (!BridgedArg)
//         continue;
//
//      BridgedArguments.push_back(BridgedArg);
//      IsBridgedArgument.set(CurIdx);
//   }
//
//   // Try to match a bridged return value.
//   BridgedReturn.match(BridgedCall);
//
//   // Don't outline inside the outlined function.
//   auto OutlinedName = getOutlinedFunctionName();
//   auto CurrentName = ObjCMethod->getFunction()->getName();
//   if (CurrentName.equals(OutlinedName))
//      return false;
//
//   // Don't outline if we created an outlined function without the bridged result
//   // from the outlined function with the bridged result (only the suffix is
//   // different: MethodNameTem...n_ vs MethodNameTem...b_).
//   if (OutlinedName.size() == CurrentName.size() &&
//       CurrentName.startswith(
//          StringRef(OutlinedName.c_str(), OutlinedName.size() - 2)))
//      return false;
//
//   return !BridgedArguments.empty();
//}
//
//CanPILFunctionType ObjCMethodCall::getOutlinedFunctionType(PILModule &M) {
//   auto FunTy = BridgedCall->getSubstCalleeType();
//   SmallVector<PILParameterInfo, 4> Parameters;
//   unsigned OrigSigIdx = 0;
//   unsigned BridgedArgIdx = 0;
//   for (auto &ParamInfo : FunTy->getParameters()) {
//      // Either use the bridged type passing it @owned.
//      if (BridgedArgIdx < BridgedArguments.size() &&
//          BridgedArguments[BridgedArgIdx].Idx == OrigSigIdx) {
//         Parameters.push_back(PILParameterInfo(BridgedArguments[BridgedArgIdx]
//                                                  .bridgedValue()
//                                                  ->getType()
//                                                  .getAstType(),
//                                               ParameterConvention::Direct_Owned));
//         ++BridgedArgIdx;
//      } else {
//         // Otherwise, use the original type convention.
//         Parameters.push_back(ParamInfo);
//      }
//      OrigSigIdx++;
//   }
//
//   auto ExtInfo = PILFunctionType::ExtInfo(
//      PILFunctionType::Representation::Thin,
//      /*pseudogeneric*/ false,
//      /*noescape*/ false,
//      DifferentiabilityKind::NonDifferentiable,
//      /*clangFunctionType*/ nullptr);
//
//   SmallVector<PILResultInfo, 4> Results;
//   // If we don't have a bridged return we changed from @autoreleased to @owned
//   // if there is a result.
//   if (!BridgedReturn) {
//      if (FunTy->getNumResults()) {
//         auto OrigResultInfo = FunTy->getSingleResult();
//         Results.push_back(PILResultInfo(OrigResultInfo.getInterfaceType(),
//                                         OrigResultInfo.getConvention() ==
//                                         ResultConvention::Autoreleased
//                                         ? ResultConvention::Owned
//                                         : OrigResultInfo.getConvention()));
//      }
//   } else {
//      // Otherwise, we used the bridged return type.
//      Results.push_back(
//         PILResultInfo(BridgedReturn.getReturnType(), ResultConvention::Owned));
//   }
//   auto FunctionType = PILFunctionType::get(
//      nullptr, ExtInfo, PILCoroutineKind::None,
//      ParameterConvention::Direct_Unowned, Parameters, {},
//      Results, None,
//      SubstitutionMap(), false,
//      M.getAstContext());
//   return FunctionType;
//}

namespace {
/// A collection of outlineable patterns.
class OutlinePatterns {
   BridgedProperty BridgedPropertyPattern;
   // @todo
//   ObjCMethodCall ObjCMethodCallPattern;
   llvm::DenseMap<CanType, PILDeclRef> BridgeToObjectiveCCache;
   llvm::DenseMap<CanType, PILDeclRef> BridgeFromObjectiveCache;

public:
   /// Try matching an outlineable pattern from the current instruction.
   OutlinePattern *tryToMatch(PILBasicBlock::iterator CurInst) {
      if (BridgedPropertyPattern.matchInstSequence(CurInst))
         return &BridgedPropertyPattern;
      // @todo
//      if (ObjCMethodCallPattern.matchInstSequence(CurInst))
//         return &ObjCMethodCallPattern;

      return nullptr;
   }

   OutlinePatterns(PILOptFunctionBuilder &FuncBuilder)
      : BridgedPropertyPattern(FuncBuilder)/*,
        ObjCMethodCallPattern(FuncBuilder)*/ {}
   ~OutlinePatterns() {}

   OutlinePatterns(const OutlinePatterns&) = delete;
   OutlinePatterns& operator=(const OutlinePatterns) = delete;
};
} // end anonymous namespace.


/// Perform outlining on the function and return any newly created outlined
/// functions.
bool tryOutline(PILOptFunctionBuilder &FuncBuilder, PILFunction *Fun,
                SmallVectorImpl<PILFunction *> &FunctionsAdded) {
   SmallPtrSet<PILBasicBlock *, 32> Visited;
   SmallVector<PILBasicBlock *, 128> Worklist;
   OutlinePatterns patterns(FuncBuilder);

   // Traverse the function.
   Worklist.push_back(&*Fun->begin());
   while (!Worklist.empty()) {

      PILBasicBlock *CurBlock = Worklist.pop_back_val();
      if (!Visited.insert(CurBlock).second) continue;

      PILBasicBlock::iterator CurInst = CurBlock->begin();

      // Go over the instructions trying to match and replace patterns.
      while (CurInst != CurBlock->end()) {
         if (OutlinePattern *match = patterns.tryToMatch(CurInst)) {
            PILFunction *F;
            PILBasicBlock::iterator LastInst;
            std::tie(F, LastInst) = match->outline(Fun->getModule());
            if (F)
               FunctionsAdded.push_back(F);
            CurInst = LastInst;
            assert(LastInst->getParent() == CurBlock);
         } else if (isa<TermInst>(CurInst)) {
            std::copy(CurBlock->succ_begin(), CurBlock->succ_end(),
                      std::back_inserter(Worklist));
            ++CurInst;
         } else {
            ++CurInst;
         }
      }
   }
   return false;
}

namespace {

class Outliner : public PILFunctionTransform {
public:
   Outliner() { }

   void run() override {
      auto *Fun = getFunction();

      // We do not support [ossa] now.
      if (Fun->hasOwnership())
         return;

      // Only outline if we optimize for size.
      if (!Fun->optimizeForSize())
         return;

      // Dump function if requested.
      if (DumpFuncsBeforeOutliner.size() &&
          Fun->getName().find(DumpFuncsBeforeOutliner, 0) != StringRef::npos) {
         Fun->dump();
      }

      PILOptFunctionBuilder FuncBuilder(*this);
      SmallVector<PILFunction *, 16> FunctionsAdded;
      bool Changed = tryOutline(FuncBuilder, Fun, FunctionsAdded);

      if (!FunctionsAdded.empty()) {
         // Notify the pass manager of any new functions we outlined.
         for (auto *AddedFunc : FunctionsAdded) {
            addFunctionToPassManagerWorklist(AddedFunc, nullptr);
         }
      }

      if (Changed) {
         invalidateAnalysis(PILAnalysis::InvalidationKind::Everything);
      }
   }
};

} //end anonymous namespace.

PILTransform *polar::createOutliner() {
   return new Outliner();
}