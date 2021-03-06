//===--- ARCAnalysis.cpp - PIL ARC Analysis -------------------------------===//
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

#define DEBUG_TYPE "pil-arc-analysis"

#include "polarphp/pil/optimizer/analysis/ARCAnalysis.h"
#include "polarphp/pil/lang/DebugUtils.h"
#include "polarphp/pil/lang/InstructionUtils.h"
#include "polarphp/pil/lang/Projection.h"
#include "polarphp/pil/lang/PILFunction.h"
#include "polarphp/pil/lang/PILInstruction.h"
#include "polarphp/pil/optimizer/analysis/AliasAnalysis.h"
#include "polarphp/pil/optimizer/analysis/RCIdentityAnalysis.h"
#include "polarphp/pil/optimizer/analysis/ValueTracking.h"
#include "polarphp/pil/optimizer/utils/InstOptUtils.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Debug.h"

using namespace polar;

using BasicBlockRetainValue = std::pair<PILBasicBlock *, PILValue>;

//===----------------------------------------------------------------------===//
//                             Utility Analysis
//===----------------------------------------------------------------------===//

bool polar::isRetainInstruction(PILInstruction *I) {
   switch (I->getKind()) {
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case PILInstructionKind::Name##RetainInst:
#include "polarphp/ast/ReferenceStorageDef.h"
      case PILInstructionKind::StrongRetainInst:
      case PILInstructionKind::RetainValueInst:
         return true;
      default:
         return false;
   }
}


bool polar::isReleaseInstruction(PILInstruction *I) {
   switch (I->getKind()) {
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case PILInstructionKind::Name##ReleaseInst:
#include "polarphp/ast/ReferenceStorageDef.h"
      case PILInstructionKind::StrongReleaseInst:
      case PILInstructionKind::ReleaseValueInst:
         return true;
      default:
         return false;
   }
}

//===----------------------------------------------------------------------===//
//                             Decrement Analysis
//===----------------------------------------------------------------------===//

bool polar::mayDecrementRefCount(PILInstruction *User,
                                 PILValue Ptr, AliasAnalysis *AA) {
   // First do a basic check, mainly based on the type of instruction.
   // Reading the RC is as "bad" as releasing.
   if (!User->mayReleaseOrReadRefCount())
      return false;

   // Ok, this instruction may have ref counts. If it is an apply, attempt to
   // prove that the callee is unable to affect Ptr.
   if (auto *AI = dyn_cast<ApplyInst>(User))
      return AA->canApplyDecrementRefCount(AI, Ptr);
   if (auto *TAI = dyn_cast<TryApplyInst>(User))
      return AA->canApplyDecrementRefCount(TAI, Ptr);
   if (auto *BI = dyn_cast<BuiltinInst>(User))
      return AA->canBuiltinDecrementRefCount(BI, Ptr);

   // We cannot conservatively prove that this instruction cannot decrement the
   // ref count of Ptr. So assume that it does.
   return true;
}

//===----------------------------------------------------------------------===//
//                                Use Analysis
//===----------------------------------------------------------------------===//

/// Returns true if a builtin apply can use reference counted values.
///
/// The main case that this handles here are builtins that via read none imply
/// that they cannot read globals and at the same time do not take any
/// non-trivial types via the arguments. The reason why we care about taking
/// non-trivial types as arguments is that we want to be careful in the face of
/// intrinsics that may be equivalent to bitcast and inttoptr operations.
static bool canApplyOfBuiltinUseNonTrivialValues(BuiltinInst *BInst) {
   auto *F = BInst->getFunction();

   auto &II = BInst->getIntrinsicInfo();
   if (II.ID != llvm::Intrinsic::not_intrinsic) {
      if (II.hasAttribute(llvm::Attribute::ReadNone)) {
         for (auto &Op : BInst->getAllOperands()) {
            if (!Op.get()->getType().isTrivial(*F)) {
               return true;
            }
         }
      }

      return false;
   }

   auto &BI = BInst->getBuiltinInfo();
   if (!BI.isReadNone())
      return true;

   for (auto &Op : BInst->getAllOperands()) {
      if (!Op.get()->getType().isTrivial(*F)) {
         return true;
      }
   }
   return false;
}

/// Returns true if \p Inst may access any indirect object either via an address
/// or reference.
///
/// If these instructions do have an address or reference type operand, then
/// they only operate on the value of the address itself, not the
/// memory. i.e. they don't dereference the address.
bool polar::canUseObject(PILInstruction *Inst) {
   switch (Inst->getKind()) {
      // These instructions do not use other values.
      case PILInstructionKind::FunctionRefInst:
      case PILInstructionKind::DynamicFunctionRefInst:
      case PILInstructionKind::PreviousDynamicFunctionRefInst:
      case PILInstructionKind::IntegerLiteralInst:
      case PILInstructionKind::FloatLiteralInst:
      case PILInstructionKind::StringLiteralInst:
      case PILInstructionKind::AllocStackInst:
      case PILInstructionKind::AllocRefInst:
      case PILInstructionKind::AllocRefDynamicInst:
      case PILInstructionKind::AllocBoxInst:
      case PILInstructionKind::MetatypeInst:
      case PILInstructionKind::WitnessMethodInst:
         return false;

         // DeallocStackInst do not use reference counted values.
      case PILInstructionKind::DeallocStackInst:
         return false;

         // Debug values do not use referenced counted values in a manner we care
         // about.
      case PILInstructionKind::DebugValueInst:
      case PILInstructionKind::DebugValueAddrInst:
         return false;

         // Casts do not use pointers in a manner that we care about since we strip
         // them during our analysis. The reason for this is if the cast is not dead
         // then there must be some other use after the cast that we will protect if a
         // release is not in between the cast and the use.
         //
         // Note: UncheckedRefCastAddrInst moves a reference into a new object. While
         // the net reference count should be zero, there's no guarantee it won't
         // access the object.
      case PILInstructionKind::UpcastInst:
      case PILInstructionKind::AddressToPointerInst:
      case PILInstructionKind::PointerToAddressInst:
      case PILInstructionKind::UncheckedRefCastInst:
      case PILInstructionKind::UncheckedAddrCastInst:
      case PILInstructionKind::RefToRawPointerInst:
      case PILInstructionKind::RawPointerToRefInst:
      case PILInstructionKind::UnconditionalCheckedCastInst:
      case PILInstructionKind::UncheckedBitwiseCastInst:
         return false;

         // If we have a trivial bit cast between trivial types, it is not something
         // that can use ref count ops in a way we care about. We do need to be careful
         // with uses with ref count inputs. In such a case, we assume conservatively
         // that the bit cast could use it.
         //
         // The reason why this is different from the ref bitcast is b/c the use of a
         // ref bit cast is still a ref typed value implying that our ARC dataflow will
         // properly handle its users. A conversion of a reference count value to a
         // trivial value though could be used as a trivial value in ways that ARC
         // dataflow will not understand implying we need to treat it as a use to be
         // safe.
      case PILInstructionKind::UncheckedTrivialBitCastInst: {
         PILValue Op = cast<UncheckedTrivialBitCastInst>(Inst)->getOperand();
         return !Op->getType().isTrivial(*Inst->getFunction());
      }

         // Typed GEPs do not use pointers. The user of the typed GEP may but we will
         // catch that via the dataflow.
      case PILInstructionKind::StructExtractInst:
      case PILInstructionKind::TupleExtractInst:
      case PILInstructionKind::StructElementAddrInst:
      case PILInstructionKind::TupleElementAddrInst:
      case PILInstructionKind::UncheckedTakeEnumDataAddrInst:
      case PILInstructionKind::RefElementAddrInst:
      case PILInstructionKind::RefTailAddrInst:
      case PILInstructionKind::UncheckedEnumDataInst:
      case PILInstructionKind::IndexAddrInst:
      case PILInstructionKind::IndexRawPointerInst:
         return false;

         // Aggregate formation by themselves do not create new uses since it is their
         // users that would create the appropriate uses.
      case PILInstructionKind::EnumInst:
      case PILInstructionKind::StructInst:
      case PILInstructionKind::TupleInst:
         return false;

         // Only uses non reference counted values.
      case PILInstructionKind::CondFailInst:
         return false;

      case PILInstructionKind::BuiltinInst: {
         auto *BI = cast<BuiltinInst>(Inst);

         // Certain builtin function refs we know can never use non-trivial values.
         return canApplyOfBuiltinUseNonTrivialValues(BI);
      }
         // We do not care about branch inst, since if the branch inst's argument is
         // dead, LLVM will clean it up.
      case PILInstructionKind::BranchInst:
      case PILInstructionKind::CondBranchInst:
         return false;
      default:
         return true;
   }
}

static bool doOperandsAlias(ArrayRef<Operand> Ops, PILValue Ptr,
                            AliasAnalysis *AA) {
   // If any are not no alias, we have a use.
   return std::any_of(Ops.begin(), Ops.end(),
                      [&AA, &Ptr](const Operand &Op) -> bool {
                         return !AA->isNoAlias(Ptr, Op.get());
                      });
}

static bool canTerminatorUseValue(TermInst *TI, PILValue Ptr,
                                  AliasAnalysis *AA) {
   if (auto *BI = dyn_cast<BranchInst>(TI)) {
      return doOperandsAlias(BI->getAllOperands(), Ptr, AA);
   }

   if (auto *CBI = dyn_cast<CondBranchInst>(TI)) {
      bool First = doOperandsAlias(CBI->getTrueOperands(), Ptr, AA);
      bool Second = doOperandsAlias(CBI->getFalseOperands(), Ptr, AA);
      return First || Second;
   }

   if (auto *SWEI = dyn_cast<SwitchEnumInst>(TI)) {
      return doOperandsAlias(SWEI->getAllOperands(), Ptr, AA);
   }

   if (auto *SWVI = dyn_cast<SwitchValueInst>(TI)) {
      return doOperandsAlias(SWVI->getAllOperands(), Ptr, AA);
   }

   auto *CCBI = dyn_cast<CheckedCastBranchInst>(TI);
   // If we don't have this last case, be conservative and assume that we can use
   // the value.
   if (!CCBI)
      return true;

   // Otherwise, look at the operands.
   return doOperandsAlias(CCBI->getAllOperands(), Ptr, AA);
}


bool polar::mayHaveSymmetricInterference(PILInstruction *User, PILValue Ptr, AliasAnalysis *AA) {
   // If Inst is an instruction that we know can never use values with reference
   // semantics, return true. Check this before AliasAnalysis because some memory
   // operations, like dealloc_stack, don't use ref counted values.
   if (!canUseObject(User))
      return false;

   // Check whether releasing this value can call deinit and interfere with User.
   if (AA->mayValueReleaseInterfereWithInstruction(User, Ptr))
      return true;

   // If the user is a load or a store and we can prove that it does not access
   // the object then return true.
   // Notice that we need to check all of the values of the object.
   if (isa<StoreInst>(User)) {
      if (AA->mayWriteToMemory(User, Ptr))
         return true;
      return false;
   }

   if (isa<LoadInst>(User) ) {
      if (AA->mayReadFromMemory(User, Ptr))
         return true;
      return false;
   }

   // If we have a terminator instruction, see if it can use ptr. This currently
   // means that we first show that TI cannot indirectly use Ptr and then use
   // alias analysis on the arguments.
   if (auto *TI = dyn_cast<TermInst>(User))
      return canTerminatorUseValue(TI, Ptr, AA);

   // TODO: If we add in alias analysis support here for apply inst, we will need
   // to check that the pointer does not escape.

   // Otherwise, assume that Inst can use Target.
   return true;
}

//===----------------------------------------------------------------------===//
//                             Must Use Analysis
//===----------------------------------------------------------------------===//

/// Returns true if User must use Ptr.
///
/// In terms of ARC this means that if we do not remove User, all releases post
/// dominated by User are known safe.
bool polar::mustUseValue(PILInstruction *User, PILValue Ptr,
                         AliasAnalysis *AA) {
   // Right now just pattern match applies.
   auto *AI = dyn_cast<ApplyInst>(User);
   if (!AI)
      return false;

   // If any of AI's arguments must alias Ptr, return true.
   for (PILValue Arg : AI->getArguments())
      if (AA->isMustAlias(Arg, Ptr))
         return true;
   return false;
}

/// Returns true if User must use Ptr in a guaranteed way.
///
/// This means that assuming that everything is conservative, we can ignore the
/// ref count effects of User on Ptr since we will only remove things over
/// guaranteed parameters if we are known safe in both directions.
bool polar::mustGuaranteedUseValue(PILInstruction *User, PILValue Ptr,
                                   AliasAnalysis *AA) {
   // Right now just pattern match applies.
   auto *AI = dyn_cast<ApplyInst>(User);
   if (!AI)
      return false;

   // For now just look for guaranteed self.
   //
   // TODO: Expand this to handle *any* guaranteed parameter.
   if (!AI->hasGuaranteedSelfArgument())
      return false;

   // Return true if Ptr alias's self.
   return AA->isMustAlias(AI->getSelfArgument(), Ptr);
}

//===----------------------------------------------------------------------===//
// Utility Methods for determining use, decrement of values in a contiguous
// instruction range in one BB.
//===----------------------------------------------------------------------===//

/// If \p Op has arc uses in the instruction range [Start, End), return the
/// first such instruction. Otherwise return None. We assume that
/// Start and End are both in the same basic block.
Optional<PILBasicBlock::iterator>
polar::
valueHasARCUsesInInstructionRange(PILValue Op,
                                  PILBasicBlock::iterator Start,
                                  PILBasicBlock::iterator End,
                                  AliasAnalysis *AA) {
   assert(Start->getParent() == End->getParent() &&
          "Start and End should be in the same basic block");

   // If Start == End, then we have an empty range, return false.
   if (Start == End)
      return None;

   // Otherwise, until Start != End.
   while (Start != End) {
      // Check if Start can use Op in an ARC relevant way. If so, return true.
      if (mayHaveSymmetricInterference(&*Start, Op, AA))
         return Start;

      // Otherwise, increment our iterator.
      ++Start;
   }

   // If all such instructions cannot use Op, return false.
   return None;
}

/// If \p Op has arc uses in the instruction range (Start, End], return the
/// first such instruction. Otherwise return None. We assume that Start and End
/// are both in the same basic block.
Optional<PILBasicBlock::iterator>
polar::valueHasARCUsesInReverseInstructionRange(PILValue Op,
                                                PILBasicBlock::iterator Start,
                                                PILBasicBlock::iterator End,
                                                AliasAnalysis *AA) {
   assert(Start->getParent() == End->getParent() &&
          "Start and End should be in the same basic block");
   assert(End != End->getParent()->end() &&
          "End should be mapped to an actual instruction");

   // If Start == End, then we have an empty range, return false.
   if (Start == End)
      return None;

   // Otherwise, until End == Start.
   while (Start != End) {
      // Check if Start can use Op in an ARC relevant way. If so, return true.
      if (mayHaveSymmetricInterference(&*End, Op, AA))
         return End;

      // Otherwise, decrement our iterator.
      --End;
   }

   // If all such instructions cannot use Op, return false.
   return None;
}

/// If \p Op has instructions in the instruction range (Start, End] which may
/// decrement it, return the first such instruction. Returns None
/// if no such instruction exists. We assume that Start and End are both in the
/// same basic block.
Optional<PILBasicBlock::iterator>
polar::
valueHasARCDecrementOrCheckInInstructionRange(PILValue Op,
                                              PILBasicBlock::iterator Start,
                                              PILBasicBlock::iterator End,
                                              AliasAnalysis *AA) {
   assert(Start->getParent() == End->getParent() &&
          "Start and End should be in the same basic block");

   // If Start == End, then we have an empty range, return nothing.
   if (Start == End)
      return None;

   // Otherwise, until Start != End.
   while (Start != End) {
      // Check if Start can decrement or check Op's ref count. If so, return
      // Start. Ref count checks do not have side effects, but are barriers for
      // retains.
      if (mayDecrementRefCount(&*Start, Op, AA) || mayCheckRefCount(&*Start))
         return Start;
      // Otherwise, increment our iterator.
      ++Start;
   }

   // If all such instructions cannot decrement Op, return nothing.
   return None;
}

bool
polar::
mayGuaranteedUseValue(PILInstruction *User, PILValue Ptr, AliasAnalysis *AA) {
   // Instructions that check the ref count are modeled as both a potential
   // decrement and a use.
   if (mayCheckRefCount(User)) {
      switch (User->getKind()) {
         case PILInstructionKind::IsUniqueInst:
            // This instruction takes the address of its referent, so there's no way
            // for the optimizer to reuse the reference across it (it appears to
            // mutate the reference itself). In fact it's operand's RC root would be
            // the parent object. This means we can ignore it as a direct RC user.
            return false;
         case PILInstructionKind::IsEscapingClosureInst:
            // FIXME: this is overly conservative. It should return true only of the
            // RC identity of the single operand matches Ptr.
            return true;
         default:
            llvm_unreachable("Unexpected check-ref-count instruction.");
      }
   }

   // Only full apply sites can require a guaranteed lifetime. If we don't have
   // one, bail.
   if (!isa<FullApplySite>(User))
      return false;

   FullApplySite FAS(User);

   // Ok, we have a full apply site. Check if the callee is callee_guaranteed. In
   // such a case, if we can not prove no alias, we need to be conservative and
   // return true.
   CanPILFunctionType FType = FAS.getSubstCalleeType();
   if (FType->isCalleeGuaranteed() && !AA->isNoAlias(FAS.getCallee(), Ptr)) {
      return true;
   }

   // Ok, we have a full apply site and our callee is a normal use. Thus if the
   // apply does not have any normal arguments, we don't need to worry about any
   // guaranteed parameters and return early.
   if (!FAS.getNumArguments())
      return false;

   // Ok, we have an apply site with arguments. Look at the function type and
   // iterate through the function parameters. If any of the parameters are
   // guaranteed, attempt to prove that the passed in parameter cannot alias
   // Ptr. If we fail, return true.
   auto Params = FType->getParameters();
   for (unsigned i : indices(Params)) {
      if (!Params[i].isGuaranteed())
         continue;
      PILValue Op = FAS.getArgument(i);
      if (!AA->isNoAlias(Op, Ptr))
         return true;
   }

   // Ok, we were able to prove that all arguments to the apply that were
   // guaranteed do not alias Ptr. Return false.
   return false;
}

//===----------------------------------------------------------------------===//
//                          Owned Result Utilities
//===----------------------------------------------------------------------===//

ConsumedResultToEpilogueRetainMatcher::
ConsumedResultToEpilogueRetainMatcher(RCIdentityFunctionInfo *RCFI,
                                      AliasAnalysis *AA,
                                      PILFunction *F)
   : F(F), RCFI(RCFI), AA(AA) {
   recompute();
}

void ConsumedResultToEpilogueRetainMatcher::recompute() {
   EpilogueRetainInsts.clear();

   // Find the return BB of F. If we fail, then bail.
   PILFunction::iterator BB = F->findReturnBB();
   if (BB == F->end())
      return;
   findMatchingRetains(&*BB);
}

bool ConsumedResultToEpilogueRetainMatcher::isTransitiveSuccessorsRetainFree(
   const llvm::DenseSet<PILBasicBlock *> &BBs) {
   // For every block with retain, we need to check the transitive
   // closure of its successors are retain-free.
   for (auto &I : EpilogueRetainInsts) {
      for (auto &Succ : I->getParent()->getSuccessors()) {
         if (BBs.count(Succ))
            continue;
         return false;
      }
   }

   // FIXME: We are iterating over a DenseSet. That can lead to non-determinism
   // and is in general pretty inefficient since we are iterating over a hash
   // table.
   for (auto CBB : BBs) {
      for (auto &Succ : CBB->getSuccessors()) {
         if (BBs.count(Succ))
            continue;
         return false;
      }
   }
   return true;
}

ConsumedResultToEpilogueRetainMatcher::RetainKindValue
ConsumedResultToEpilogueRetainMatcher::
findMatchingRetainsInBasicBlock(PILBasicBlock *BB, PILValue V) {
   for (auto II = BB->rbegin(), IE = BB->rend(); II != IE; ++II) {
      // Handle self-recursion.
      if (auto *AI = dyn_cast<ApplyInst>(&*II))
         if (AI->getCalleeFunction() == BB->getParent())
            return std::make_pair(FindRetainKind::Recursion, AI);

      // If we do not have a retain_value or strong_retain...
      if (!isa<RetainValueInst>(*II) && !isa<StrongRetainInst>(*II)) {
         // we can ignore it if it can not decrement the reference count of the
         // return value.
         if (!mayDecrementRefCount(&*II, V, AA))
            continue;

         // Otherwise, we need to stop computing since we do not want to create
         // lifetime gap.
         return std::make_pair(FindRetainKind::Blocked, nullptr);
      }

      // Ok, we have a retain_value or strong_retain. Grab Target and find the
      // RC identity root of its operand.
      PILInstruction *Target = &*II;
      PILValue RetainValue = RCFI->getRCIdentityRoot(Target->getOperand(0));
      PILValue ReturnValue = RCFI->getRCIdentityRoot(V);

      // Is this the epilogue retain we are looking for ?.
      // We break here as we do not know whether this is a part of the epilogue
      // retain for the @own return value.
      if (RetainValue != ReturnValue)
         break;

      return std::make_pair(FindRetainKind::Found, &*II);
   }

   // Did not find retain in this block.
   return std::make_pair(FindRetainKind::None, nullptr);
}

void
ConsumedResultToEpilogueRetainMatcher::
findMatchingRetains(PILBasicBlock *BB) {
   // Iterate over the instructions post-order and find retains associated with
   // return value.
   PILValue RV = PILValue();
   for (auto II = BB->rbegin(), IE = BB->rend(); II != IE; ++II) {
      if (auto *RI = dyn_cast<ReturnInst>(&*II)) {
         RV = RI->getOperand();
         break;
      }
   }

   // Somehow, we managed not to find a return value.
   if (!RV)
      return;

   // OK. we've found the return value, now iterate on the CFG to find all the
   // post-dominating retains.
   //
   // The ConsumedResultToEpilogueRetainMatcher finds the final releases
   // in the following way.
   //
   // 1. If an instruction, which is not releaseinst nor releasevalue, that
   // could decrement reference count is found. bail out.
   //
   // 2. If a release is found and the release that can not be mapped to any
   // @owned argument. bail as this release may well be the final release of
   // an @owned argument, but somehow rc-identity fails to prove that.
   //
   // 3. A release that is mapped to an argument which already has a release
   // that overlaps with this release. This release for sure is not the final
   // release.
   constexpr unsigned WorkListMaxSize = 4;

   llvm::DenseSet<PILBasicBlock *> RetainFrees;
   llvm::SmallVector<BasicBlockRetainValue, 4> WorkList;
   llvm::DenseSet<PILBasicBlock *> HandledBBs;
   WorkList.push_back(std::make_pair(BB, RV));
   HandledBBs.insert(BB);
   while (!WorkList.empty()) {
      // Too many blocks ?.
      if (WorkList.size() > WorkListMaxSize) {
         EpilogueRetainInsts.clear();
         return;
      }

      // Try to find a retain %value in this basic block.
      auto R = WorkList.pop_back_val();
      RetainKindValue Kind = findMatchingRetainsInBasicBlock(R.first, R.second);

      // We've found a retain on this path.
      if (Kind.first == FindRetainKind::Found) {
         EpilogueRetainInsts.push_back(Kind.second);
         continue;
      }

      // There is a MayDecrement instruction.
      if (Kind.first == FindRetainKind::Blocked) {
         EpilogueRetainInsts.clear();
         return;
      }

      // There is a self-recursion. Use the apply instruction as the retain.
      if (Kind.first == FindRetainKind::Recursion) {
         EpilogueRetainInsts.push_back(Kind.second);
         continue;
      }

      // Did not find a retain in this block, try to go to its predecessors.
      if (Kind.first == FindRetainKind::None) {
         // We can not find a retain in a block with no predecessors.
         if (R.first->getPredecessorBlocks().begin() ==
             R.first->getPredecessorBlocks().end()) {
            EpilogueRetainInsts.clear();
            return;
         }
         // This block does not have a retain.
         RetainFrees.insert(R.first);
         // If this is a PILArgument of current basic block, we can split it up to
         // values in the predecessors.
         auto *SA = dyn_cast<PILPhiArgument>(R.second);
         if (SA && SA->getParent() != R.first)
            SA = nullptr;
         for (auto X : R.first->getPredecessorBlocks()) {
            if (HandledBBs.find(X) != HandledBBs.end())
               continue;
            // Try to use the predecessor edge-value.
            if (SA && SA->getIncomingPhiValue(X)) {
               WorkList.push_back(std::make_pair(X, SA->getIncomingPhiValue(X)));
            } else
               WorkList.push_back(std::make_pair(X, R.second));
            HandledBBs.insert(X);
         }
      }
   }

   // Lastly, check whether all the successor blocks are retain-free.
   if (!isTransitiveSuccessorsRetainFree(RetainFrees))
      EpilogueRetainInsts.clear();

   // At this point, we've either failed to find any epilogue retains or
   // all the post-dominating epilogue retains.
}

//===----------------------------------------------------------------------===//
//                          Owned Argument Utilities
//===----------------------------------------------------------------------===//

ConsumedArgToEpilogueReleaseMatcher::ConsumedArgToEpilogueReleaseMatcher(
   RCIdentityFunctionInfo *RCFI,
   PILFunction *F,
   ArrayRef<PILArgumentConvention> ArgumentConventions,
   ExitKind Kind)
   : F(F), RCFI(RCFI), Kind(Kind), ArgumentConventions(ArgumentConventions),
     ProcessedBlock(nullptr) {
   recompute();
}

void ConsumedArgToEpilogueReleaseMatcher::recompute() {
   ArgInstMap.clear();

   // Find the return BB of F. If we fail, then bail.
   PILFunction::iterator BB;
   switch (Kind) {
      case ExitKind::Return:
         BB = F->findReturnBB();
         break;
      case ExitKind::Throw:
         BB = F->findThrowBB();
         break;
   }

   if (BB == F->end()) {
      ProcessedBlock = nullptr;
      return;
   }
   ProcessedBlock = &*BB;
   findMatchingReleases(&*BB);
}

bool ConsumedArgToEpilogueReleaseMatcher::isRedundantRelease(
   ArrayRef<PILInstruction *> Insts, PILValue Base, PILValue Derived) {
   // We use projection path to analyze the relation.
   auto POp = ProjectionPath::getProjectionPath(Base, Derived);
   // We can not build a projection path from the base to the derived, bail out.
   // and return true so that we can stop the epilogue walking sequence.
   if (!POp.hasValue())
      return true;

   for (auto &R : Insts) {
      PILValue ROp = R->getOperand(0);
      auto PROp = ProjectionPath::getProjectionPath(Base, ROp);
      if (!PROp.hasValue())
         return true;
      // If Op is a part of ROp or Rop is a part of Op. then we have seen
      // a redundant release.
      if (!PROp.getValue().hasNonEmptySymmetricDifference(POp.getValue()))
         return true;
   }
   return false;
}

bool ConsumedArgToEpilogueReleaseMatcher::releaseArgument(
   ArrayRef<PILInstruction *> Insts, PILValue Arg) {
   // Reason about whether all parts are released.
   auto *F = (*Insts.begin())->getFunction();

   // These are the list of PILValues that are actually released.
   ProjectionPathSet Paths;
   for (auto &I : Insts) {
      auto PP = ProjectionPath::getProjectionPath(Arg, I->getOperand(0));
      if (!PP)
         return false;
      Paths.insert(PP.getValue());
   }

   // Is there an uncovered non-trivial type.
   return !ProjectionPath::hasUncoveredNonTrivials(Arg->getType(), *F, Paths);
}

void
ConsumedArgToEpilogueReleaseMatcher::
processMatchingReleases() {
   // If we can not find a release for all parts with reference semantics
   // that means we did not find all releases for the base.
   for (auto &pair : ArgInstMap) {
      // We do not know if we have a fully post dominating release set
      // so all release sets should be considered partially post
      // dominated.
      auto releaseSet = pair.second.getPartiallyPostDomReleases();
      if (!releaseSet)
         continue;

      // If an argument has a single release and it is rc-identical to the
      // PILArgument. Then we do not need to use projection to check for whether
      // all non-trivial fields are covered.
      if (releaseSet->size() == 1) {
         PILInstruction *inst = *releaseSet->begin();
         PILValue rv = inst->getOperand(0);
         if (pair.first == RCFI->getRCIdentityRoot(rv)) {
            pair.second.setHasJointPostDominatingReleaseSet();
            continue;
         }
      }

      // OK. we have multiple epilogue releases for this argument, check whether
      // it has covered all fields with reference semantic in the argument.
      if (!releaseArgument(*releaseSet, pair.first))
         continue;

      // OK. At this point we know that we found a joint post dominating
      // set of releases. Mark our argument as such.
      pair.second.setHasJointPostDominatingReleaseSet();
   }
}

/// Check if a given argument convention is in the list
/// of possible argument conventions.
static bool
isOneOfConventions(PILArgumentConvention Convention,
                   ArrayRef<PILArgumentConvention> ArgumentConventions) {
   for (auto ArgumentConvention : ArgumentConventions) {
      if (Convention == ArgumentConvention)
         return true;
   }
   return false;
}

void ConsumedArgToEpilogueReleaseMatcher::collectMatchingDestroyAddresses(
   PILBasicBlock *block) {
   // Check if we can find destroy_addr for each @in argument.
   PILFunction::iterator anotherEpilogueBB =
      (Kind == ExitKind::Return) ? F->findThrowBB() : F->findReturnBB();

   for (auto *arg : F->begin()->getPILFunctionArguments()) {
      if (arg->isIndirectResult())
         continue;
      if (arg->getArgumentConvention() != PILArgumentConvention::Indirect_In)
         continue;
      bool hasDestroyAddrOutsideEpilogueBB = false;
      // This is an @in argument. Check if there are any destroy_addr
      // instructions for it.
      for (Operand *op : getNonDebugUses(arg)) {
         auto *user = op->getUser();
         if (!isa<DestroyAddrInst>(user))
            continue;
         // Do not take into account any uses in the other
         // epilogue BB.
         if (anotherEpilogueBB != F->end() &&
             user->getParent() == &*anotherEpilogueBB)
            continue;
         if (user->getParent() != block)
            hasDestroyAddrOutsideEpilogueBB = true;

         // Since ArgumentState uses a TinyPtrVector, creating
         // temporaries containing one element is cheap.
         auto iter = ArgInstMap.insert({arg, ArgumentState(user)});
         // We inserted the value.
         if (iter.second)
            continue;
         // Otherwise, add this release to the set.
         iter.first->second.addRelease(user);
      }

      // Don't know how to handle destroy_addr outside of the epilogue.
      if (hasDestroyAddrOutsideEpilogueBB)
         ArgInstMap.erase(arg);
   }
}

void ConsumedArgToEpilogueReleaseMatcher::collectMatchingReleases(
   PILBasicBlock *block) {
   // Iterate over the instructions post-order and find final releases
   // associated with each arguments.
   //
   // The ConsumedArgToEpilogueReleaseMatcher finds the final releases
   // in the following way.
   //
   // 1. If an instruction, which is not releaseinst nor releasevalue, that
   // could decrement reference count is found. bail out.
   //
   // 2. If a release is found and the release that can not be mapped to any
   // @owned argument. bail as this release may well be the final release of
   // an @owned argument, but somehow rc-identity fails to prove that.
   //
   // 3. A release that is mapped to an argument which already has a release
   // that overlaps with this release. This release for sure is not the final
   // release.
   bool isTrackingInArgs = isOneOfConventions(PILArgumentConvention::Indirect_In,
                                              ArgumentConventions);
   for (auto &inst : llvm::reverse(*block)) {
      if (isTrackingInArgs && isa<DestroyAddrInst>(inst)) {
         // It is probably a destroy addr for an @in argument.
         continue;
      }
      // If we do not have a release_value or strong_release. We can continue
      if (!isa<ReleaseValueInst>(inst) && !isa<StrongReleaseInst>(inst)) {
         // We cannot match a final release if it is followed by a dealloc_ref.
         if (isa<DeallocRefInst>(inst))
            break;

         // We do not know what this instruction is, do a simple check to make sure
         // that it does not decrement the reference count of any of its operand.
         //
         // TODO: we could make the logic here more complicated to handle each type
         // of instructions in a more precise manner.
         if (!inst.mayRelease())
            continue;
         // This instruction may release something, bail out conservatively.
         break;
      }

      // Ok, we have a release_value or strong_release. Grab Target and find the
      // RC identity root of its operand.
      PILValue origOp = inst.getOperand(0);
      PILValue op = RCFI->getRCIdentityRoot(origOp);

      // Check whether this is a PILArgument or a part of a PILArgument. This is
      // possible after we expand release instructions in PILLowerAgg pass.
      auto *arg = dyn_cast<PILFunctionArgument>(stripValueProjections(op));
      if (!arg)
         break;

      // If Op is not a consumed argument, we must break since this is not an Op
      // that is a part of a return sequence. We are being conservative here since
      // we could make this more general by allowing for intervening non-arg
      // releases in the sense that we do not allow for race conditions in between
      // destructors.
      if (!arg ||
          !isOneOfConventions(arg->getArgumentConvention(), ArgumentConventions))
         break;

      // Ok, we have a release on a PILArgument that has a consuming convention.
      // Attempt to put it into our arc opts map. If we already have it, we have
      // exited the return value sequence so break. Otherwise, continue looking
      // for more arc operations.
      auto iter = ArgInstMap.find(arg);
      if (iter == ArgInstMap.end()) {
         ArgInstMap.insert({arg, {&inst}});
         continue;
      }

      // We've already seen at least part of this base. Check to see whether we
      // are seeing a redundant release.
      //
      // If we are seeing a redundant release we have exited the return value
      // sequence, so break.
      if (!isa<DestroyAddrInst>(inst)) {
         // We do not know if we have a fully post dominating release
         // set, so we use the partial post dom entry point.
         if (auto partialReleases = iter->second.getPartiallyPostDomReleases()) {
            if (isRedundantRelease(*partialReleases, arg, origOp)) {
               break;
            }
         }
      }

      // We've seen part of this base, but this is a part we've have not seen.
      // Record it.
      iter->second.addRelease(&inst);
   }

   if (isTrackingInArgs) {
      // Find destroy_addr for each @in argument.
      collectMatchingDestroyAddresses(block);
   }
}

void
ConsumedArgToEpilogueReleaseMatcher::
findMatchingReleases(PILBasicBlock *BB) {
   // Walk the given basic block to find all the epilogue releases.
   collectMatchingReleases(BB);
   // We've exited the epilogue sequence, try to find out which parameter we
   // have all the epilogue releases for and which one we did not.
   processMatchingReleases();
}

//===----------------------------------------------------------------------===//
//                    Code for Determining Final Releases
//===----------------------------------------------------------------------===//

// Propagate liveness backwards from an initial set of blocks in our
// LiveIn set.
static void propagateLiveness(llvm::SmallPtrSetImpl<PILBasicBlock *> &LiveIn,
                              PILBasicBlock *DefBB) {
   // First populate a worklist of predecessors.
   llvm::SmallVector<PILBasicBlock *, 64> Worklist;
   for (auto *BB : LiveIn)
      for (auto Pred : BB->getPredecessorBlocks())
         Worklist.push_back(Pred);

   // Now propagate liveness backwards until we hit the alloc_box.
   while (!Worklist.empty()) {
      auto *BB = Worklist.pop_back_val();

      // If it's already in the set, then we've already queued and/or
      // processed the predecessors.
      if (BB == DefBB || !LiveIn.insert(BB).second)
         continue;

      for (auto Pred : BB->getPredecessorBlocks())
         Worklist.push_back(Pred);
   }
}

// Is any successor of BB in the LiveIn set?
static bool successorHasLiveIn(PILBasicBlock *BB,
                               llvm::SmallPtrSetImpl<PILBasicBlock *> &LiveIn) {
   for (auto &Succ : BB->getSuccessors())
      if (LiveIn.count(Succ))
         return true;

   return false;
}

// Walk backwards in BB looking for the last use of a given
// value, and add it to the set of release points.
static bool addLastUse(PILValue V, PILBasicBlock *BB,
                       ReleaseTracker &Tracker) {
   for (auto I = BB->rbegin(); I != BB->rend(); ++I) {
      if (Tracker.isUser(&*I)) {
         Tracker.trackLastRelease(&*I);
         return true;
      }
   }

   llvm_unreachable("BB is expected to have a use of a closure");
   return false;
}

/// TODO: Refactor this code so the decision on whether or not to accept an
/// instruction.
bool polar::getFinalReleasesForValue(PILValue V, ReleaseTracker &Tracker) {
   llvm::SmallPtrSet<PILBasicBlock *, 16> LiveIn;
   llvm::SmallPtrSet<PILBasicBlock *, 16> UseBlocks;

   // First attempt to get the BB where this value resides.
   auto *DefBB = V->getParentBlock();
   if (!DefBB)
      return false;

   bool seenRelease = false;
   PILInstruction *OneRelease = nullptr;

   // We'll treat this like a liveness problem where the value is the def. Each
   // block that has a use of the value has the value live-in unless it is the
   // block with the value.
   SmallVector<Operand *, 8> Uses(V->getUses());
   while (!Uses.empty()) {
      auto *Use = Uses.pop_back_val();
      auto *User = Use->getUser();
      auto *BB = User->getParent();

      if (Tracker.isUserTransitive(User)) {
         Tracker.trackUser(User);
         auto *CastInst = cast<SingleValueInstruction>(User);
         Uses.append(CastInst->getUses().begin(), CastInst->getUses().end());
         continue;
      }

      if (!Tracker.isUserAcceptable(User))
         return false;

      Tracker.trackUser(User);

      if (BB != DefBB)
         LiveIn.insert(BB);

      // Also keep track of the blocks with uses.
      UseBlocks.insert(BB);

      // Try to speed up the trivial case of single release/dealloc.
      if (isa<StrongReleaseInst>(User) || isa<DeallocBoxInst>(User) ||
          isa<DestroyValueInst>(User) || isa<ReleaseValueInst>(User)) {
         if (!seenRelease)
            OneRelease = User;
         else
            OneRelease = nullptr;

         seenRelease = true;
      }
   }

   // Only a single release/dealloc? We're done!
   if (OneRelease) {
      Tracker.trackLastRelease(OneRelease);
      return true;
   }

   propagateLiveness(LiveIn, DefBB);

   // Now examine each block we saw a use in. If it has no successors
   // that are in LiveIn, then the last use in the block is the final
   // release/dealloc.
   for (auto *BB : UseBlocks)
      if (!successorHasLiveIn(BB, LiveIn))
         if (!addLastUse(V, BB, Tracker))
            return false;

   return true;
}

//===----------------------------------------------------------------------===//
//                            Leaking BB Analysis
//===----------------------------------------------------------------------===//

static bool ignorableApplyInstInUnreachableBlock(const ApplyInst *AI) {
   auto applySite = FullApplySite(const_cast<ApplyInst *>(AI));
   return applySite.isCalleeKnownProgramTerminationPoint();
}

static bool ignorableBuiltinInstInUnreachableBlock(const BuiltinInst *BI) {
   const BuiltinInfo &BInfo = BI->getBuiltinInfo();
   if (BInfo.ID == BuiltinValueKind::CondUnreachable)
      return true;

   const IntrinsicInfo &IInfo = BI->getIntrinsicInfo();
   if (IInfo.ID == llvm::Intrinsic::trap)
      return true;

   return false;
}

/// Match a call to a trap BB with no ARC relevant side effects.
bool polar::isARCInertTrapBB(const PILBasicBlock *BB) {
   // Do a quick check at the beginning to make sure that our terminator is
   // actually an unreachable. This ensures that in many cases this function will
   // exit early and quickly.
   auto II = BB->rbegin();
   if (!isa<UnreachableInst>(*II))
      return false;

   auto IE = BB->rend();
   while (II != IE) {
      // Ignore any instructions without side effects.
      if (!II->mayHaveSideEffects()) {
         ++II;
         continue;
      }

      // Ignore cond fail.
      if (isa<CondFailInst>(*II)) {
         ++II;
         continue;
      }

      // Check for apply insts that we can ignore.
      if (auto *AI = dyn_cast<ApplyInst>(&*II)) {
         if (ignorableApplyInstInUnreachableBlock(AI)) {
            ++II;
            continue;
         }
      }

      // Check for builtins that we can ignore.
      if (auto *BI = dyn_cast<BuiltinInst>(&*II)) {
         if (ignorableBuiltinInstInUnreachableBlock(BI)) {
            ++II;
            continue;
         }
      }

      // If we can't ignore the instruction, return false.
      return false;
   }

   // Otherwise, we have an unreachable and every instruction is inert from an
   // ARC perspective in an unreachable BB.
   return true;
}

//===----------------------------------------------------------------------===//
//             Analysis of builtin "unsafeGuaranteed" instructions
//===----------------------------------------------------------------------===//
std::pair<SingleValueInstruction *, SingleValueInstruction *>
polar::getSingleUnsafeGuaranteedValueResult(BuiltinInst *BI) {
   assert(BI->getBuiltinKind() &&
          *BI->getBuiltinKind() == BuiltinValueKind::UnsafeGuaranteed &&
          "Expecting a unsafeGuaranteed builtin");

   SingleValueInstruction *GuaranteedValue = nullptr;
   SingleValueInstruction *Token = nullptr;

   auto Failed = std::make_pair(nullptr, nullptr);

   for (auto *Operand : getNonDebugUses(BI)) {
      auto *Usr = Operand->getUser();
      if (isa<ReleaseValueInst>(Usr) || isa<RetainValueInst>(Usr))
         continue;

      auto *TE = dyn_cast<TupleExtractInst>(Usr);
      if (!TE || TE->getOperand() != BI)
         return Failed;

      if (TE->getFieldNo() == 0 && !GuaranteedValue) {
         GuaranteedValue = TE;
         continue;
      }
      if (TE->getFieldNo() == 1 && !Token) {
         Token = TE;
         continue;
      }
      return Failed;
   }

   if (!GuaranteedValue || !Token)
      return Failed;

   return std::make_pair(GuaranteedValue, Token);
}

BuiltinInst *polar::getUnsafeGuaranteedEndUser(PILValue UnsafeGuaranteedToken) {
   BuiltinInst *UnsafeGuaranteedEndI = nullptr;

   for (auto *Operand : getNonDebugUses(UnsafeGuaranteedToken)) {
      if (UnsafeGuaranteedEndI) {
         LLVM_DEBUG(llvm::dbgs() << "  multiple unsafeGuaranteedEnd users\n");
         UnsafeGuaranteedEndI = nullptr;
         break;
      }
      auto *BI = dyn_cast<BuiltinInst>(Operand->getUser());
      if (!BI || !BI->getBuiltinKind() ||
          *BI->getBuiltinKind() != BuiltinValueKind::UnsafeGuaranteedEnd) {
         LLVM_DEBUG(llvm::dbgs() << "  wrong unsafeGuaranteed token user "
                                 << *Operand->getUser());
         break;
      }

      UnsafeGuaranteedEndI = BI;
   }
   return UnsafeGuaranteedEndI;
}

static bool hasUnsafeGuaranteedOperand(PILValue UnsafeGuaranteedValue,
                                       PILValue UnsafeGuaranteedValueOperand,
                                       RCIdentityFunctionInfo &RCII,
                                       PILInstruction &Release) {
   assert((isa<StrongReleaseInst>(Release) ||
           isa<ReleaseValueInst>(Release)) && "Expecting a release");

   auto RCRoot = RCII.getRCIdentityRoot(Release.getOperand(0));

   return RCRoot == UnsafeGuaranteedValue ||
          RCRoot == UnsafeGuaranteedValueOperand;
}

PILInstruction *polar::findReleaseToMatchUnsafeGuaranteedValue(
   PILInstruction *UnsafeGuaranteedEndI, PILInstruction *UnsafeGuaranteedI,
   PILValue UnsafeGuaranteedValue, PILBasicBlock &BB,
   RCIdentityFunctionInfo &RCFI) {

   auto UnsafeGuaranteedRoot = RCFI.getRCIdentityRoot(UnsafeGuaranteedValue);
   auto UnsafeGuaranteedOpdRoot =
      RCFI.getRCIdentityRoot(UnsafeGuaranteedI->getOperand(0));

   // Try finding it after the "unsafeGuaranteedEnd".
   for (auto ForwardIt = std::next(UnsafeGuaranteedEndI->getIterator()),
           End = BB.end();
        ForwardIt != End; ++ForwardIt) {
      PILInstruction &CurInst = *ForwardIt;

      // Is this a release?
      if (isa<ReleaseValueInst>(CurInst) || isa<StrongReleaseInst>(CurInst)) {
         if (hasUnsafeGuaranteedOperand(UnsafeGuaranteedRoot,
                                        UnsafeGuaranteedOpdRoot, RCFI, CurInst))
            return &CurInst;
         continue;
      }

      if (CurInst.mayHaveSideEffects() && !isa<DebugValueInst>(CurInst) &&
          !isa<DebugValueAddrInst>(CurInst))
         break;
   }

   // Otherwise, Look before the "unsafeGuaranteedEnd".
   for (auto ReverseIt = ++UnsafeGuaranteedEndI->getIterator().getReverse(),
           End = BB.rend();
        ReverseIt != End; ++ReverseIt) {
      PILInstruction &CurInst = *ReverseIt;

      // Is this a release?
      if (isa<ReleaseValueInst>(CurInst) || isa<StrongReleaseInst>(CurInst)) {
         if (hasUnsafeGuaranteedOperand(UnsafeGuaranteedRoot,
                                        UnsafeGuaranteedOpdRoot, RCFI, CurInst))
            return &CurInst;
         continue;
      }
      if (CurInst.mayHaveSideEffects() && !isa<DebugValueInst>(CurInst) &&
          !isa<DebugValueAddrInst>(CurInst))
         break;
   }
   return nullptr;
}

