//===----------------- LoopRotationUtils.cpp -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides utilities to convert a loop into a loop with bottom test.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/LoopRotationUtils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
using namespace llvm;

#define DEBUG_TYPE "loop-rotate"

STATISTIC(NumNotRotatedDueToHeaderSize,
          "Number of loops not rotated due to the header size");
STATISTIC(NumRotated, "Number of loops rotated");

static cl::opt<bool>
    MultiRotate("loop-rotate-multi", cl::init(false), cl::Hidden,
                cl::desc("Allow loop rotation multiple times in order to reach "
                         "a better latch exit"));

namespace {
/// A simple loop rotation transformation.
class LoopRotate {
  const unsigned MaxHeaderSize;
  LoopInfo *LI;
  const TargetTransformInfo *TTI;
  AssumptionCache *AC;
  DominatorTree *DT;
  ScalarEvolution *SE;
  MemorySSAUpdater *MSSAU;
  const SimplifyQuery &SQ;
  bool RotationOnly;
  bool IsUtilMode;
  bool PrepareForLTO;

public:
  LoopRotate(unsigned MaxHeaderSize, LoopInfo *LI,
             const TargetTransformInfo *TTI, AssumptionCache *AC,
             DominatorTree *DT, ScalarEvolution *SE, MemorySSAUpdater *MSSAU,
             const SimplifyQuery &SQ, bool RotationOnly, bool IsUtilMode,
             bool PrepareForLTO)
      : MaxHeaderSize(MaxHeaderSize), LI(LI), TTI(TTI), AC(AC), DT(DT), SE(SE),
        MSSAU(MSSAU), SQ(SQ), RotationOnly(RotationOnly),
        IsUtilMode(IsUtilMode), PrepareForLTO(PrepareForLTO) {}
  bool processLoop(Loop *L);

private:
  bool rotateLoop(Loop *L, bool SimplifiedLatch);
  bool simplifyLoopLatch(Loop *L);
};
} // end anonymous namespace

/// Insert (K, V) pair into the ValueToValueMap, and verify the key did not
/// previously exist in the map, and the value was inserted.
static void InsertNewValueIntoMap(ValueToValueMapTy &VM, Value *K, Value *V) {
  bool Inserted = VM.insert({K, V}).second;
  assert(Inserted);
  (void)Inserted;
}
/// RewriteUsesOfClonedInstructions - We just cloned the instructions from the
/// old header into the preheader.  If there were uses of the values produced by
/// these instruction that were outside of the loop, we have to insert PHI nodes
/// to merge the two values.  Do this now.
static void RewriteUsesOfClonedInstructions(BasicBlock *OrigHeader,
                                            BasicBlock *OrigPreheader,
                                            ValueToValueMapTy &ValueMap,
                                SmallVectorImpl<PHINode*> *InsertedPHIs) {
  // Remove PHI node entries that are no longer live.
  BasicBlock::iterator I, E = OrigHeader->end();
  for (I = OrigHeader->begin(); PHINode *PN = dyn_cast<PHINode>(I); ++I)
    PN->removeIncomingValue(PN->getBasicBlockIndex(OrigPreheader));

  // Now fix up users of the instructions in OrigHeader, inserting PHI nodes
  // as necessary.
  SSAUpdater SSA(InsertedPHIs);
  for (I = OrigHeader->begin(); I != E; ++I) {
    Value *OrigHeaderVal = &*I;

    // If there are no uses of the value (e.g. because it returns void), there
    // is nothing to rewrite.
    if (OrigHeaderVal->use_empty())
      continue;

    Value *OrigPreHeaderVal = ValueMap.lookup(OrigHeaderVal);

    // The value now exits in two versions: the initial value in the preheader
    // and the loop "next" value in the original header.
    SSA.Initialize(OrigHeaderVal->getType(), OrigHeaderVal->getName());
    SSA.AddAvailableValue(OrigHeader, OrigHeaderVal);
    SSA.AddAvailableValue(OrigPreheader, OrigPreHeaderVal);

    // Visit each use of the OrigHeader instruction.
    for (Value::use_iterator UI = OrigHeaderVal->use_begin(),
                             UE = OrigHeaderVal->use_end();
         UI != UE;) {
      // Grab the use before incrementing the iterator.
      Use &U = *UI;

      // Increment the iterator before removing the use from the list.
      ++UI;

      // SSAUpdater can't handle a non-PHI use in the same block as an
      // earlier def. We can easily handle those cases manually.
      Instruction *UserInst = cast<Instruction>(U.getUser());
      if (!isa<PHINode>(UserInst)) {
        BasicBlock *UserBB = UserInst->getParent();

        // The original users in the OrigHeader are already using the
        // original definitions.
        if (UserBB == OrigHeader)
          continue;

        // Users in the OrigPreHeader need to use the value to which the
        // original definitions are mapped.
        if (UserBB == OrigPreheader) {
          U = OrigPreHeaderVal;
          continue;
        }
      }

      // Anything else can be handled by SSAUpdater.
      SSA.RewriteUse(U);
    }

    // Replace MetadataAsValue(ValueAsMetadata(OrigHeaderVal)) uses in debug
    // intrinsics.
    SmallVector<DbgValueInst *, 1> DbgValues;
    llvm::findDbgValues(DbgValues, OrigHeaderVal);
    for (auto &DbgValue : DbgValues) {
      // The original users in the OrigHeader are already using the original
      // definitions.
      BasicBlock *UserBB = DbgValue->getParent();
      if (UserBB == OrigHeader)
        continue;

      // Users in the OrigPreHeader need to use the value to which the
      // original definitions are mapped and anything else can be handled by
      // the SSAUpdater. To avoid adding PHINodes, check if the value is
      // available in UserBB, if not substitute undef.
      Value *NewVal;
      if (UserBB == OrigPreheader)
        NewVal = OrigPreHeaderVal;
      else if (SSA.HasValueForBlock(UserBB))
        NewVal = SSA.GetValueInMiddleOfBlock(UserBB);
      else
        NewVal = UndefValue::get(OrigHeaderVal->getType());
      DbgValue->setOperand(0,
                           MetadataAsValue::get(OrigHeaderVal->getContext(),
                                                ValueAsMetadata::get(NewVal)));
    }
  }
}

// Assuming both header and latch are exiting, look for a phi which is only
// used outside the loop (via a LCSSA phi) in the exit from the header.
// This means that rotating the loop can remove the phi.
static bool profitableToRotateLoopExitingLatch(Loop *L) {
  BasicBlock *Header = L->getHeader();
  BranchInst *BI = dyn_cast<BranchInst>(Header->getTerminator());
  assert(BI && BI->isConditional() && "need header with conditional exit");
  BasicBlock *HeaderExit = BI->getSuccessor(0);
  if (L->contains(HeaderExit))
    HeaderExit = BI->getSuccessor(1);

  for (auto &Phi : Header->phis()) {
    // Look for uses of this phi in the loop/via exits other than the header.
    if (llvm::any_of(Phi.users(), [HeaderExit](const User *U) {
          return cast<Instruction>(U)->getParent() != HeaderExit;
        }))
      continue;
    return true;
  }
  return false;
}

// Check that latch exit is deoptimizing (which means - very unlikely to happen)
// and there is another exit from the loop which is non-deoptimizing.
// If we rotate latch to that exit our loop has a better chance of being fully
// canonical.
//
// It can give false positives in some rare cases.
static bool canRotateDeoptimizingLatchExit(Loop *L) {
  BasicBlock *Latch = L->getLoopLatch();
  assert(Latch && "need latch");
  BranchInst *BI = dyn_cast<BranchInst>(Latch->getTerminator());
  // Need normal exiting latch.
  if (!BI || !BI->isConditional())
    return false;

  BasicBlock *Exit = BI->getSuccessor(1);
  if (L->contains(Exit))
    Exit = BI->getSuccessor(0);

  // Latch exit is non-deoptimizing, no need to rotate.
  if (!Exit->getPostdominatingDeoptimizeCall())
    return false;

  SmallVector<BasicBlock *, 4> Exits;
  L->getUniqueExitBlocks(Exits);
  if (!Exits.empty()) {
    // There is at least one non-deoptimizing exit.
    //
    // Note, that BasicBlock::getPostdominatingDeoptimizeCall is not exact,
    // as it can conservatively return false for deoptimizing exits with
    // complex enough control flow down to deoptimize call.
    //
    // That means here we can report success for a case where
    // all exits are deoptimizing but one of them has complex enough
    // control flow (e.g. with loops).
    //
    // That should be a very rare case and false positives for this function
    // have compile-time effect only.
    return any_of(Exits, [](const BasicBlock *BB) {
      return !BB->getPostdominatingDeoptimizeCall();
    });
  }
  return false;
}

/// Rotate loop LP. Return true if the loop is rotated.
///
/// \param SimplifiedLatch is true if the latch was just folded into the final
/// loop exit. In this case we may want to rotate even though the new latch is
/// now an exiting branch. This rotation would have happened had the latch not
/// been simplified. However, if SimplifiedLatch is false, then we avoid
/// rotating loops in which the latch exits to avoid excessive or endless
/// rotation. LoopRotate should be repeatable and converge to a canonical
/// form. This property is satisfied because simplifying the loop latch can only
/// happen once across multiple invocations of the LoopRotate pass.
///
/// If -loop-rotate-multi is enabled we can do multiple rotations in one go
/// so to reach a suitable (non-deoptimizing) exit.
bool LoopRotate::rotateLoop(Loop *L, bool SimplifiedLatch) {
  // If the loop has only one block then there is not much to rotate.
  if (L->getBlocks().size() == 1)
    return false;

  bool Rotated = false;
  do {
    BasicBlock *OrigHeader = L->getHeader();
    BasicBlock *OrigLatch = L->getLoopLatch();

    BranchInst *BI = dyn_cast<BranchInst>(OrigHeader->getTerminator());
    if (!BI || BI->isUnconditional())
      return Rotated;

    // If the loop header is not one of the loop exiting blocks then
    // either this loop is already rotated or it is not
    // suitable for loop rotation transformations.
    if (!L->isLoopExiting(OrigHeader))
      return Rotated;

    // If the loop latch already contains a branch that leaves the loop then the
    // loop is already rotated.
    if (!OrigLatch)
      return Rotated;

    // Rotate if either the loop latch does *not* exit the loop, or if the loop
    // latch was just simplified. Or if we think it will be profitable.
    if (L->isLoopExiting(OrigLatch) && !SimplifiedLatch && IsUtilMode == false &&
        !profitableToRotateLoopExitingLatch(L) &&
        !canRotateDeoptimizingLatchExit(L))
      return Rotated;

    // Check size of original header and reject loop if it is very big or we can't
    // duplicate blocks inside it.
    {
      SmallPtrSet<const Value *, 32> EphValues;
      CodeMetrics::collectEphemeralValues(L, AC, EphValues);

      CodeMetrics Metrics;
      Metrics.analyzeBasicBlock(OrigHeader, *TTI, EphValues, PrepareForLTO);
      if (Metrics.notDuplicatable) {
        LLVM_DEBUG(
                   dbgs() << "LoopRotation: NOT rotating - contains non-duplicatable"
                   << " instructions: ";
                   L->dump());
        return Rotated;
      }
      if (Metrics.convergent) {
        LLVM_DEBUG(dbgs() << "LoopRotation: NOT rotating - contains convergent "
                   "instructions: ";
                   L->dump());
        return Rotated;
      }
      if (Metrics.NumInsts > MaxHeaderSize) {
        LLVM_DEBUG(dbgs() << "LoopRotation: NOT rotating - contains "
                          << Metrics.NumInsts
                          << " instructions, which is more than the threshold ("
                          << MaxHeaderSize << " instructions): ";
                   L->dump());
        ++NumNotRotatedDueToHeaderSize;
        return Rotated;
      }

      // When preparing for LTO, avoid rotating loops with calls that could be
      // inlined during the LTO stage.
      if (PrepareForLTO && Metrics.NumInlineCandidates > 0)
        return Rotated;
    }

    // Now, this loop is suitable for rotation.
    BasicBlock *OrigPreheader = L->getLoopPreheader();

    // If the loop could not be converted to canonical form, it must have an
    // indirectbr in it, just give up.
    if (!OrigPreheader || !L->hasDedicatedExits())
      return Rotated;

    // Anything ScalarEvolution may know about this loop or the PHI nodes
    // in its header will soon be invalidated. We should also invalidate
    // all outer loops because insertion and deletion of blocks that happens
    // during the rotation may violate invariants related to backedge taken
    // infos in them.
    if (SE)
      SE->forgetTopmostLoop(L);

    LLVM_DEBUG(dbgs() << "LoopRotation: rotating "; L->dump());
    if (MSSAU && VerifyMemorySSA)
      MSSAU->getMemorySSA()->verifyMemorySSA();

    // Find new Loop header. NewHeader is a Header's one and only successor
    // that is inside loop.  Header's other successor is outside the
    // loop.  Otherwise loop is not suitable for rotation.
    BasicBlock *Exit = BI->getSuccessor(0);
    BasicBlock *NewHeader = BI->getSuccessor(1);
    if (L->contains(Exit))
      std::swap(Exit, NewHeader);
    assert(NewHeader && "Unable to determine new loop header");
    assert(L->contains(NewHeader) && !L->contains(Exit) &&
           "Unable to determine loop header and exit blocks");

    // This code assumes that the new header has exactly one predecessor.
    // Remove any single-entry PHI nodes in it.
    assert(NewHeader->getSinglePredecessor() &&
           "New header doesn't have one pred!");
    FoldSingleEntryPHINodes(NewHeader);

    // Begin by walking OrigHeader and populating ValueMap with an entry for
    // each Instruction.
    BasicBlock::iterator I = OrigHeader->begin(), E = OrigHeader->end();
    ValueToValueMapTy ValueMap, ValueMapMSSA;

    // For PHI nodes, the value available in OldPreHeader is just the
    // incoming value from OldPreHeader.
    for (; PHINode *PN = dyn_cast<PHINode>(I); ++I)
      InsertNewValueIntoMap(ValueMap, PN,
                            PN->getIncomingValueForBlock(OrigPreheader));

    // For the rest of the instructions, either hoist to the OrigPreheader if
    // possible or create a clone in the OldPreHeader if not.
    Instruction *LoopEntryBranch = OrigPreheader->getTerminator();

    // Record all debug intrinsics preceding LoopEntryBranch to avoid duplication.
    using DbgIntrinsicHash =
      std::pair<std::pair<Value *, DILocalVariable *>, DIExpression *>;
    auto makeHash = [](DbgVariableIntrinsic *D) -> DbgIntrinsicHash {
      return {{D->getVariableLocation(), D->getVariable()}, D->getExpression()};
    };
    SmallDenseSet<DbgIntrinsicHash, 8> DbgIntrinsics;
    for (auto I = std::next(OrigPreheader->rbegin()), E = OrigPreheader->rend();
         I != E; ++I) {
      if (auto *DII = dyn_cast<DbgVariableIntrinsic>(&*I))
        DbgIntrinsics.insert(makeHash(DII));
      else
        break;
    }

    // Remember the local noalias scope declarations in the header. After the
    // rotation, they must be duplicated and the scope must be cloned. This
    // avoids unwanted interaction across iterations.
    SmallVector<Instruction *, 6> NoAliasDeclInstructions;
    for (Instruction &I : *OrigHeader)
      if (auto *Decl = dyn_cast<NoAliasScopeDeclInst>(&I))
        NoAliasDeclInstructions.push_back(Decl);

    while (I != E) {
      Instruction *Inst = &*I++;

      // If the instruction's operands are invariant and it doesn't read or write
      // memory, then it is safe to hoist.  Doing this doesn't change the order of
      // execution in the preheader, but does prevent the instruction from
      // executing in each iteration of the loop.  This means it is safe to hoist
      // something that might trap, but isn't safe to hoist something that reads
      // memory (without proving that the loop doesn't write).
      if (L->hasLoopInvariantOperands(Inst) && !Inst->mayReadFromMemory() &&
          !Inst->mayWriteToMemory() && !Inst->isTerminator() &&
          !isa<DbgInfoIntrinsic>(Inst) && !isa<AllocaInst>(Inst)) {
        Inst->moveBefore(LoopEntryBranch);
        continue;
      }

      // Otherwise, create a duplicate of the instruction.
      Instruction *C = Inst->clone();

      // Eagerly remap the operands of the instruction.
      RemapInstruction(C, ValueMap,
                       RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);

      // Avoid inserting the same intrinsic twice.
      if (auto *DII = dyn_cast<DbgVariableIntrinsic>(C))
        if (DbgIntrinsics.count(makeHash(DII))) {
          C->deleteValue();
          continue;
        }

      // With the operands remapped, see if the instruction constant folds or is
      // otherwise simplifyable.  This commonly occurs because the entry from PHI
      // nodes allows icmps and other instructions to fold.
      Value *V = SimplifyInstruction(C, SQ);
      if (V && LI->replacementPreservesLCSSAForm(C, V)) {
        // If so, then delete the temporary instruction and stick the folded value
        // in the map.
        InsertNewValueIntoMap(ValueMap, Inst, V);
        if (!C->mayHaveSideEffects()) {
          C->deleteValue();
          C = nullptr;
        }
      } else {
        InsertNewValueIntoMap(ValueMap, Inst, C);
      }
      if (C) {
        // Otherwise, stick the new instruction into the new block!
        C->setName(Inst->getName());
        C->insertBefore(LoopEntryBranch);

        if (auto *II = dyn_cast<IntrinsicInst>(C))
          if (II->getIntrinsicID() == Intrinsic::assume)
            AC->registerAssumption(II);
        // MemorySSA cares whether the cloned instruction was inserted or not, and
        // not whether it can be remapped to a simplified value.
        if (MSSAU)
          InsertNewValueIntoMap(ValueMapMSSA, Inst, C);
      }
    }

    if (!NoAliasDeclInstructions.empty()) {
      // There are noalias scope declarations:
      // (general):
      // Original:    OrigPre              { OrigHeader NewHeader ... Latch }
      // after:      (OrigPre+OrigHeader') { NewHeader ... Latch OrigHeader }
      //
      // with D: llvm.experimental.noalias.scope.decl,
      //      U: !noalias or !alias.scope depending on D
      //       ... { D U1 U2 }   can transform into:
      // (0) : ... { D U1 U2 }        // no relevant rotation for this part
      // (1) : ... D' { U1 U2 D }     // D is part of OrigHeader
      // (2) : ... D' U1' { U2 D U1 } // D, U1 are part of OrigHeader
      //
      // We now want to transform:
      // (1) -> : ... D' { D U1 U2 D'' }
      // (2) -> : ... D' U1' { D U2 D'' U1'' }
      // D: original llvm.experimental.noalias.scope.decl
      // D', U1': duplicate with replaced scopes
      // D'', U1'': different duplicate with replaced scopes
      // This ensures a safe fallback to 'may_alias' introduced by the rotate,
      // as U1'' and U1' scopes will not be compatible wrt to the local restrict

      // Clone the llvm.experimental.noalias.decl again for the NewHeader.
      Instruction *NewHeaderInsertionPoint = &(*NewHeader->getFirstNonPHI());
      for (Instruction *NAD : NoAliasDeclInstructions) {
        LLVM_DEBUG(dbgs() << "  Cloning llvm.experimental.noalias.scope.decl:"
                          << *NAD << "\n");
        Instruction *NewNAD = NAD->clone();
        NewNAD->insertBefore(NewHeaderInsertionPoint);
      }

      // Scopes must now be duplicated, once for OrigHeader and once for
      // OrigPreHeader'.
      {
        auto &Context = NewHeader->getContext();

        SmallVector<MetadataAsValue *, 8> NoAliasDeclScopes;
        for (Instruction *NAD : NoAliasDeclInstructions)
          NoAliasDeclScopes.push_back(cast<MetadataAsValue>(
              NAD->getOperand(Intrinsic::NoAliasScopeDeclScopeArg)));

        LLVM_DEBUG(dbgs() << "  Updating OrigHeader scopes\n");
        cloneAndAdaptNoAliasScopes(NoAliasDeclScopes, {OrigHeader}, Context,
                                   "h.rot");
        LLVM_DEBUG(OrigHeader->dump());

        // Keep the compile time impact low by only adapting the inserted block
        // of instructions in the OrigPreHeader. This might result in slightly
        // more aliasing between these instructions and those that were already
        // present, but it will be much faster when the original PreHeader is
        // large.
        LLVM_DEBUG(dbgs() << "  Updating part of OrigPreheader scopes\n");
        auto *FirstDecl =
            cast<Instruction>(ValueMap[*NoAliasDeclInstructions.begin()]);
        auto *LastInst = &OrigPreheader->back();
        cloneAndAdaptNoAliasScopes(NoAliasDeclScopes, FirstDecl, LastInst,
                                   Context, "pre.rot");
        LLVM_DEBUG(OrigPreheader->dump());

        LLVM_DEBUG(dbgs() << "  Updated NewHeader:\n");
        LLVM_DEBUG(NewHeader->dump());
      }
    }

    // Along with all the other instructions, we just cloned OrigHeader's
    // terminator into OrigPreHeader. Fix up the PHI nodes in each of OrigHeader's
    // successors by duplicating their incoming values for OrigHeader.
    for (BasicBlock *SuccBB : successors(OrigHeader))
      for (BasicBlock::iterator BI = SuccBB->begin();
           PHINode *PN = dyn_cast<PHINode>(BI); ++BI)
        PN->addIncoming(PN->getIncomingValueForBlock(OrigHeader), OrigPreheader);

    // Now that OrigPreHeader has a clone of OrigHeader's terminator, remove
    // OrigPreHeader's old terminator (the original branch into the loop), and
    // remove the corresponding incoming values from the PHI nodes in OrigHeader.
    LoopEntryBranch->eraseFromParent();

    // Update MemorySSA before the rewrite call below changes the 1:1
    // instruction:cloned_instruction_or_value mapping.
    if (MSSAU) {
      InsertNewValueIntoMap(ValueMapMSSA, OrigHeader, OrigPreheader);
      MSSAU->updateForClonedBlockIntoPred(OrigHeader, OrigPreheader,
                                          ValueMapMSSA);
    }

    SmallVector<PHINode*, 2> InsertedPHIs;
    // If there were any uses of instructions in the duplicated block outside the
    // loop, update them, inserting PHI nodes as required
    RewriteUsesOfClonedInstructions(OrigHeader, OrigPreheader, ValueMap,
                                    &InsertedPHIs);

    // Attach dbg.value intrinsics to the new phis if that phi uses a value that
    // previously had debug metadata attached. This keeps the debug info
    // up-to-date in the loop body.
    if (!InsertedPHIs.empty())
      insertDebugValuesForPHIs(OrigHeader, InsertedPHIs);

    // NewHeader is now the header of the loop.
    L->moveToHeader(NewHeader);
    assert(L->getHeader() == NewHeader && "Latch block is our new header");

    // Inform DT about changes to the CFG.
    if (DT) {
      // The OrigPreheader branches to the NewHeader and Exit now. Then, inform
      // the DT about the removed edge to the OrigHeader (that got removed).
      SmallVector<DominatorTree::UpdateType, 3> Updates;
      Updates.push_back({DominatorTree::Insert, OrigPreheader, Exit});
      Updates.push_back({DominatorTree::Insert, OrigPreheader, NewHeader});
      Updates.push_back({DominatorTree::Delete, OrigPreheader, OrigHeader});

      if (MSSAU) {
        MSSAU->applyUpdates(Updates, *DT, /*UpdateDT=*/true);
        if (VerifyMemorySSA)
          MSSAU->getMemorySSA()->verifyMemorySSA();
      } else {
        DT->applyUpdates(Updates);
      }
    }

    // At this point, we've finished our major CFG changes.  As part of cloning
    // the loop into the preheader we've simplified instructions and the
    // duplicated conditional branch may now be branching on a constant.  If it is
    // branching on a constant and if that constant means that we enter the loop,
    // then we fold away the cond branch to an uncond branch.  This simplifies the
    // loop in cases important for nested loops, and it also means we don't have
    // to split as many edges.
    BranchInst *PHBI = cast<BranchInst>(OrigPreheader->getTerminator());
    assert(PHBI->isConditional() && "Should be clone of BI condbr!");
    if (!isa<ConstantInt>(PHBI->getCondition()) ||
        PHBI->getSuccessor(cast<ConstantInt>(PHBI->getCondition())->isZero()) !=
        NewHeader) {
      // The conditional branch can't be folded, handle the general case.
      // Split edges as necessary to preserve LoopSimplify form.

      // Right now OrigPreHeader has two successors, NewHeader and ExitBlock, and
      // thus is not a preheader anymore.
      // Split the edge to form a real preheader.
      BasicBlock *NewPH = SplitCriticalEdge(
                                            OrigPreheader, NewHeader,
                                            CriticalEdgeSplittingOptions(DT, LI, MSSAU).setPreserveLCSSA());
      NewPH->setName(NewHeader->getName() + ".lr.ph");

      // Preserve canonical loop form, which means that 'Exit' should have only
      // one predecessor. Note that Exit could be an exit block for multiple
      // nested loops, causing both of the edges to now be critical and need to
      // be split.
      SmallVector<BasicBlock *, 4> ExitPreds(pred_begin(Exit), pred_end(Exit));
      bool SplitLatchEdge = false;
      for (BasicBlock *ExitPred : ExitPreds) {
        // We only need to split loop exit edges.
        Loop *PredLoop = LI->getLoopFor(ExitPred);
        if (!PredLoop || PredLoop->contains(Exit) ||
            ExitPred->getTerminator()->isIndirectTerminator())
          continue;
        SplitLatchEdge |= L->getLoopLatch() == ExitPred;
        BasicBlock *ExitSplit = SplitCriticalEdge(
                                                  ExitPred, Exit,
                                                  CriticalEdgeSplittingOptions(DT, LI, MSSAU).setPreserveLCSSA());
        ExitSplit->moveBefore(Exit);
      }
      assert(SplitLatchEdge &&
             "Despite splitting all preds, failed to split latch exit?");
    } else {
      // We can fold the conditional branch in the preheader, this makes things
      // simpler. The first step is to remove the extra edge to the Exit block.
      Exit->removePredecessor(OrigPreheader, true /*preserve LCSSA*/);
      BranchInst *NewBI = BranchInst::Create(NewHeader, PHBI);
      NewBI->setDebugLoc(PHBI->getDebugLoc());
      PHBI->eraseFromParent();

      // With our CFG finalized, update DomTree if it is available.
      if (DT) DT->deleteEdge(OrigPreheader, Exit);

      // Update MSSA too, if available.
      if (MSSAU)
        MSSAU->removeEdge(OrigPreheader, Exit);
    }

    assert(L->getLoopPreheader() && "Invalid loop preheader after loop rotation");
    assert(L->getLoopLatch() && "Invalid loop latch after loop rotation");

    if (MSSAU && VerifyMemorySSA)
      MSSAU->getMemorySSA()->verifyMemorySSA();

    // Now that the CFG and DomTree are in a consistent state again, try to merge
    // the OrigHeader block into OrigLatch.  This will succeed if they are
    // connected by an unconditional branch.  This is just a cleanup so the
    // emitted code isn't too gross in this common case.
    DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Eager);
    BasicBlock *PredBB = OrigHeader->getUniquePredecessor();
    bool DidMerge = MergeBlockIntoPredecessor(OrigHeader, &DTU, LI, MSSAU);
    if (DidMerge)
      RemoveRedundantDbgInstrs(PredBB);

    if (MSSAU && VerifyMemorySSA)
      MSSAU->getMemorySSA()->verifyMemorySSA();

    LLVM_DEBUG(dbgs() << "LoopRotation: into "; L->dump());

    ++NumRotated;

    Rotated = true;
    SimplifiedLatch = false;

    // Check that new latch is a deoptimizing exit and then repeat rotation if possible.
    // Deoptimizing latch exit is not a generally typical case, so we just loop over.
    // TODO: if it becomes a performance bottleneck extend rotation algorithm
    // to handle multiple rotations in one go.
  } while (MultiRotate && canRotateDeoptimizingLatchExit(L));


  return true;
}

/// Determine whether the instructions in this range may be safely and cheaply
/// speculated. This is not an important enough situation to develop complex
/// heuristics. We handle a single arithmetic instruction along with any type
/// conversions.
static bool shouldSpeculateInstrs(BasicBlock::iterator Begin,
                                  BasicBlock::iterator End, Loop *L) {
  bool seenIncrement = false;
  bool MultiExitLoop = false;

  if (!L->getExitingBlock())
    MultiExitLoop = true;

  for (BasicBlock::iterator I = Begin; I != End; ++I) {

    if (!isSafeToSpeculativelyExecute(&*I))
      return false;

    if (isa<DbgInfoIntrinsic>(I))
      continue;

    switch (I->getOpcode()) {
    default:
      return false;
    case Instruction::GetElementPtr:
      // GEPs are cheap if all indices are constant.
      if (!cast<GEPOperator>(I)->hasAllConstantIndices())
        return false;
      // fall-thru to increment case
      LLVM_FALLTHROUGH;
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr: {
      Value *IVOpnd =
          !isa<Constant>(I->getOperand(0))
              ? I->getOperand(0)
              : !isa<Constant>(I->getOperand(1)) ? I->getOperand(1) : nullptr;
      if (!IVOpnd)
        return false;

      // If increment operand is used outside of the loop, this speculation
      // could cause extra live range interference.
      if (MultiExitLoop) {
        for (User *UseI : IVOpnd->users()) {
          auto *UserInst = cast<Instruction>(UseI);
          if (!L->contains(UserInst))
            return false;
        }
      }

      if (seenIncrement)
        return false;
      seenIncrement = true;
      break;
    }
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
      // ignore type conversions
      break;
    }
  }
  return true;
}

/// Fold the loop tail into the loop exit by speculating the loop tail
/// instructions. Typically, this is a single post-increment. In the case of a
/// simple 2-block loop, hoisting the increment can be much better than
/// duplicating the entire loop header. In the case of loops with early exits,
/// rotation will not work anyway, but simplifyLoopLatch will put the loop in
/// canonical form so downstream passes can handle it.
///
/// I don't believe this invalidates SCEV.
bool LoopRotate::simplifyLoopLatch(Loop *L) {
  BasicBlock *Latch = L->getLoopLatch();
  if (!Latch || Latch->hasAddressTaken())
    return false;

  BranchInst *Jmp = dyn_cast<BranchInst>(Latch->getTerminator());
  if (!Jmp || !Jmp->isUnconditional())
    return false;

  BasicBlock *LastExit = Latch->getSinglePredecessor();
  if (!LastExit || !L->isLoopExiting(LastExit))
    return false;

  BranchInst *BI = dyn_cast<BranchInst>(LastExit->getTerminator());
  if (!BI)
    return false;

  if (!shouldSpeculateInstrs(Latch->begin(), Jmp->getIterator(), L))
    return false;

  LLVM_DEBUG(dbgs() << "Folding loop latch " << Latch->getName() << " into "
                    << LastExit->getName() << "\n");

  DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Eager);
  MergeBlockIntoPredecessor(Latch, &DTU, LI, MSSAU, nullptr,
                            /*PredecessorWithTwoSuccessors=*/true);

  if (MSSAU && VerifyMemorySSA)
    MSSAU->getMemorySSA()->verifyMemorySSA();

  return true;
}

/// Rotate \c L, and return true if any modification was made.
bool LoopRotate::processLoop(Loop *L) {
  // Save the loop metadata.
  MDNode *LoopMD = L->getLoopID();

  bool SimplifiedLatch = false;

  // Simplify the loop latch before attempting to rotate the header
  // upward. Rotation may not be needed if the loop tail can be folded into the
  // loop exit.
  if (!RotationOnly)
    SimplifiedLatch = simplifyLoopLatch(L);

  bool MadeChange = rotateLoop(L, SimplifiedLatch);
  assert((!MadeChange || L->isLoopExiting(L->getLoopLatch())) &&
         "Loop latch should be exiting after loop-rotate.");

  // Restore the loop metadata.
  // NB! We presume LoopRotation DOESN'T ADD its own metadata.
  if ((MadeChange || SimplifiedLatch) && LoopMD)
    L->setLoopID(LoopMD);

  return MadeChange || SimplifiedLatch;
}


/// The utility to convert a loop into a loop with bottom test.
bool llvm::LoopRotation(Loop *L, LoopInfo *LI, const TargetTransformInfo *TTI,
                        AssumptionCache *AC, DominatorTree *DT,
                        ScalarEvolution *SE, MemorySSAUpdater *MSSAU,
                        const SimplifyQuery &SQ, bool RotationOnly = true,
                        unsigned Threshold = unsigned(-1),
                        bool IsUtilMode = true, bool PrepareForLTO) {
  LoopRotate LR(Threshold, LI, TTI, AC, DT, SE, MSSAU, SQ, RotationOnly,
                IsUtilMode, PrepareForLTO);
  return LR.processLoop(L);
}
