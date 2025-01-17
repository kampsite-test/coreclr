// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

//
//
//                                    CopyProp
//
// This stage performs value numbering based copy propagation. Since copy propagation
// is about data flow, we cannot find them in assertion prop phase. In assertion prop
// we can identify copies, like so: if (a == b) else, i.e., control flow assertions.
//
// To identify data flow copies, we'll follow a similar approach to SSA renaming.
// We would walk each path in the graph keeping track of every live definition. Thus
// when we see a variable that shares the VN with a live definition, we'd replace this
// variable with the variable in the live definition, if suitable.
//
///////////////////////////////////////////////////////////////////////////////////////

#include "jitpch.h"
#include "ssabuilder.h"
#include "treelifeupdater.h"

/**************************************************************************************
 *
 * Corresponding to the live definition pushes, pop the stack as we finish a sub-paths
 * of the graph originating from the block. Refer SSA renaming for any additional info.
 * "curSsaName" tracks the currently live definitions.
 */
void Compiler::optBlockCopyPropPopStacks(BasicBlock* block, LclNumToGenTreePtrStack* curSsaName)
{
    for (GenTreeStmt* stmt = block->firstStmt(); stmt != nullptr; stmt = stmt->getNextStmt())
    {
        for (GenTree* tree = stmt->gtStmtList; tree != nullptr; tree = tree->gtNext)
        {
            if (!tree->IsLocal())
            {
                continue;
            }
            unsigned lclNum = tree->gtLclVarCommon.gtLclNum;
            if (!lvaInSsa(lclNum))
            {
                continue;
            }
            if (tree->gtFlags & GTF_VAR_DEF)
            {
                GenTreePtrStack* stack = nullptr;
                curSsaName->Lookup(lclNum, &stack);
                stack->Pop();
                if (stack->Empty())
                {
                    curSsaName->Remove(lclNum);
                }
            }
        }
    }
}

#ifdef DEBUG
void Compiler::optDumpCopyPropStack(LclNumToGenTreePtrStack* curSsaName)
{
    JITDUMP("{ ");
    for (LclNumToGenTreePtrStack::KeyIterator iter = curSsaName->Begin(); !iter.Equal(curSsaName->End()); ++iter)
    {
        GenTree* node = iter.GetValue()->Top();
        JITDUMP("%d-[%06d]:V%02u ", iter.Get(), dspTreeID(node), node->AsLclVarCommon()->gtLclNum);
    }
    JITDUMP("}\n\n");
}
#endif
/*******************************************************************************************************
 *
 * Given the "lclVar" and "copyVar" compute if the copy prop will be beneficial.
 *
 */
int Compiler::optCopyProp_LclVarScore(LclVarDsc* lclVarDsc, LclVarDsc* copyVarDsc, bool preferOp2)
{
    int score = 0;

    if (lclVarDsc->lvVolatileHint)
    {
        score += 4;
    }

    if (copyVarDsc->lvVolatileHint)
    {
        score -= 4;
    }

    if (lclVarDsc->lvDoNotEnregister)
    {
        score += 4;
    }

    if (copyVarDsc->lvDoNotEnregister)
    {
        score -= 4;
    }

#ifdef _TARGET_X86_
    // For doubles we also prefer to change parameters into non-parameter local variables
    if (lclVarDsc->lvType == TYP_DOUBLE)
    {
        if (lclVarDsc->lvIsParam)
        {
            score += 2;
        }

        if (copyVarDsc->lvIsParam)
        {
            score -= 2;
        }
    }
#endif

    // Otherwise we prefer to use the op2LclNum
    return score + ((preferOp2) ? 1 : -1);
}

//------------------------------------------------------------------------------
// optCopyProp : Perform copy propagation on a given tree as we walk the graph and if it is a local
//               variable, then look up all currently live definitions and check if any of those
//               definitions share the same value number. If so, then we can make the replacement.
//
// Arguments:
//    block       -  Block the tree belongs to
//    stmt        -  Statement the tree belongs to
//    tree        -  The tree to perform copy propagation on
//    curSsaName  -  The map from lclNum to its recently live definitions as a stack

void Compiler::optCopyProp(BasicBlock* block, GenTreeStmt* stmt, GenTree* tree, LclNumToGenTreePtrStack* curSsaName)
{
    // TODO-Review: EH successor/predecessor iteration seems broken.
    if (block->bbCatchTyp == BBCT_FINALLY || block->bbCatchTyp == BBCT_FAULT)
    {
        return;
    }

    // If not local nothing to do.
    if (!tree->IsLocal())
    {
        return;
    }
    if (tree->OperGet() == GT_PHI_ARG || tree->OperGet() == GT_LCL_FLD)
    {
        return;
    }

    // Propagate only on uses.
    if (tree->gtFlags & GTF_VAR_DEF)
    {
        return;
    }
    unsigned lclNum = tree->AsLclVarCommon()->GetLclNum();

    // Skip non-SSA variables.
    if (!lvaInSsa(lclNum))
    {
        return;
    }

    assert(tree->gtVNPair.GetConservative() != ValueNumStore::NoVN);

    for (LclNumToGenTreePtrStack::KeyIterator iter = curSsaName->Begin(); !iter.Equal(curSsaName->End()); ++iter)
    {
        unsigned newLclNum = iter.Get();

        GenTree* op = iter.GetValue()->Top();

        // Nothing to do if same.
        if (lclNum == newLclNum)
        {
            continue;
        }

        // Skip variables with assignments embedded in the statement (i.e., with a comma). Because we
        // are not currently updating their SSA names as live in the copy-prop pass of the stmt.
        if (VarSetOps::IsMember(this, optCopyPropKillSet, lvaTable[newLclNum].lvVarIndex))
        {
            continue;
        }

        // Do not copy propagate if the old and new lclVar have different 'doNotEnregister' settings.
        // This is primarily to avoid copy propagating to IND(ADDR(LCL_VAR)) where the replacement lclVar
        // is not marked 'lvDoNotEnregister'.
        // However, in addition, it may not be profitable to propagate a 'doNotEnregister' lclVar to an
        // existing use of an enregisterable lclVar.

        if (lvaTable[lclNum].lvDoNotEnregister != lvaTable[newLclNum].lvDoNotEnregister)
        {
            continue;
        }

        if (op->gtFlags & GTF_VAR_CAST)
        {
            continue;
        }
        if (gsShadowVarInfo != nullptr && lvaTable[newLclNum].lvIsParam &&
            gsShadowVarInfo[newLclNum].shadowCopy == lclNum)
        {
            continue;
        }
        ValueNum opVN = GetUseAsgDefVNOrTreeVN(op);
        if (opVN == ValueNumStore::NoVN)
        {
            continue;
        }
        if (op->TypeGet() != tree->TypeGet())
        {
            continue;
        }
        if (opVN != tree->gtVNPair.GetConservative())
        {
            continue;
        }
        if (optCopyProp_LclVarScore(&lvaTable[lclNum], &lvaTable[newLclNum], true) <= 0)
        {
            continue;
        }
        // Check whether the newLclNum is live before being substituted. Otherwise, we could end
        // up in a situation where there must've been a phi node that got pruned because the variable
        // is not live anymore. For example,
        //  if
        //     x0 = 1
        //  else
        //     x1 = 2
        //  print(c) <-- x is not live here. Let's say 'c' shares the value number with "x0."
        //
        // If we simply substituted 'c' with "x0", we would be wrong. Ideally, there would be a phi
        // node x2 = phi(x0, x1) which can then be used to substitute 'c' with. But because of pruning
        // there would be no such phi node. To solve this we'll check if 'x' is live, before replacing
        // 'c' with 'x.'
        if (!lvaTable[newLclNum].lvVerTypeInfo.IsThisPtr())
        {
            if (lvaTable[newLclNum].lvAddrExposed)
            {
                continue;
            }

            // We compute liveness only on tracked variables. So skip untracked locals.
            if (!lvaTable[newLclNum].lvTracked)
            {
                continue;
            }

            // Because of this dependence on live variable analysis, CopyProp phase is immediately
            // after Liveness, SSA and VN.
            if (!VarSetOps::IsMember(this, compCurLife, lvaTable[newLclNum].lvVarIndex))
            {
                continue;
            }
        }
        unsigned newSsaNum = SsaConfig::RESERVED_SSA_NUM;
        if (op->gtFlags & GTF_VAR_DEF)
        {
            newSsaNum = GetSsaNumForLocalVarDef(op);
        }
        else // parameters, this pointer etc.
        {
            newSsaNum = op->AsLclVarCommon()->GetSsaNum();
        }

        if (newSsaNum == SsaConfig::RESERVED_SSA_NUM)
        {
            continue;
        }

#ifdef DEBUG
        if (verbose)
        {
            JITDUMP("VN based copy assertion for ");
            printTreeID(tree);
            printf(" V%02d @%08X by ", lclNum, tree->GetVN(VNK_Conservative));
            printTreeID(op);
            printf(" V%02d @%08X.\n", newLclNum, op->GetVN(VNK_Conservative));
            gtDispTree(tree, nullptr, nullptr, true);
        }
#endif

        tree->gtLclVarCommon.SetLclNum(newLclNum);
        tree->AsLclVarCommon()->SetSsaNum(newSsaNum);
        gtUpdateSideEffects(stmt, tree);
#ifdef DEBUG
        if (verbose)
        {
            printf("copy propagated to:\n");
            gtDispTree(tree, nullptr, nullptr, true);
        }
#endif
        break;
    }
    return;
}

/**************************************************************************************
 *
 * Helper to check if tree is a local that participates in SSA numbering.
 */
bool Compiler::optIsSsaLocal(GenTree* tree)
{
    return tree->IsLocal() && lvaInSsa(tree->AsLclVarCommon()->GetLclNum());
}

//------------------------------------------------------------------------------
// optBlockCopyProp : Perform copy propagation using currently live definitions on the current block's
//                    variables. Also as new definitions are encountered update the "curSsaName" which
//                    tracks the currently live definitions.
//
// Arguments:
//    block       -  Block the tree belongs to
//    curSsaName  -  The map from lclNum to its recently live definitions as a stack

void Compiler::optBlockCopyProp(BasicBlock* block, LclNumToGenTreePtrStack* curSsaName)
{
#ifdef DEBUG
    JITDUMP("Copy Assertion for " FMT_BB "\n", block->bbNum);
    if (verbose)
    {
        printf("  curSsaName stack: ");
        optDumpCopyPropStack(curSsaName);
    }
#endif

    TreeLifeUpdater<false> treeLifeUpdater(this);

    // There are no definitions at the start of the block. So clear it.
    compCurLifeTree = nullptr;
    VarSetOps::Assign(this, compCurLife, block->bbLiveIn);
    for (GenTreeStmt* stmt = block->firstStmt(); stmt != nullptr; stmt = stmt->getNextStmt())
    {
        VarSetOps::ClearD(this, optCopyPropKillSet);

        // Walk the tree to find if any local variable can be replaced with current live definitions.
        for (GenTree* tree = stmt->gtStmtList; tree != nullptr; tree = tree->gtNext)
        {
            treeLifeUpdater.UpdateLife(tree);

            optCopyProp(block, stmt, tree, curSsaName);

            // TODO-Review: Merge this loop with the following loop to correctly update the
            // live SSA num while also propagating copies.
            //
            // 1. This loop performs copy prop with currently live (on-top-of-stack) SSA num.
            // 2. The subsequent loop maintains a stack for each lclNum with
            //    currently active SSA numbers when definitions are encountered.
            //
            // If there is an embedded definition using a "comma" in a stmt, then the currently
            // live SSA number will get updated only in the next loop (2). However, this new
            // definition is now supposed to be live (on tos). If we did not update the stacks
            // using (2), copy prop (1) will use a SSA num defined outside the stmt ignoring the
            // embedded update. Killing the variable is a simplification to produce 0 ASM diffs
            // for an update release.
            //
            if (optIsSsaLocal(tree) && (tree->gtFlags & GTF_VAR_DEF))
            {
                VarSetOps::AddElemD(this, optCopyPropKillSet, lvaTable[tree->gtLclVarCommon.gtLclNum].lvVarIndex);
            }
        }

        // This logic must be in sync with SSA renaming process.
        for (GenTree* tree = stmt->gtStmtList; tree != nullptr; tree = tree->gtNext)
        {
            if (!optIsSsaLocal(tree))
            {
                continue;
            }

            unsigned lclNum = tree->gtLclVarCommon.gtLclNum;

            // As we encounter a definition add it to the stack as a live definition.
            if (tree->gtFlags & GTF_VAR_DEF)
            {
                GenTreePtrStack* stack;
                if (!curSsaName->Lookup(lclNum, &stack))
                {
                    stack = new (curSsaName->GetAllocator()) GenTreePtrStack(curSsaName->GetAllocator());
                }
                stack->Push(tree);
                curSsaName->Set(lclNum, stack, LclNumToGenTreePtrStack::Overwrite);
            }
            // If we encounter first use of a param or this pointer add it as a live definition.
            // Since they are always live, do it only once.
            else if ((tree->gtOper == GT_LCL_VAR) && !(tree->gtFlags & GTF_VAR_USEASG) &&
                     (lvaTable[lclNum].lvIsParam || lvaTable[lclNum].lvVerTypeInfo.IsThisPtr()))
            {
                GenTreePtrStack* stack;
                if (!curSsaName->Lookup(lclNum, &stack))
                {
                    stack = new (curSsaName->GetAllocator()) GenTreePtrStack(curSsaName->GetAllocator());
                    stack->Push(tree);
                    curSsaName->Set(lclNum, stack);
                }
            }
        }
    }
}

/**************************************************************************************
 *
 * This stage performs value numbering based copy propagation. Since copy propagation
 * is about data flow, we cannot find them in assertion prop phase. In assertion prop
 * we can identify copies that like so: if (a == b) else, i.e., control flow assertions.
 *
 * To identify data flow copies, we follow a similar approach to SSA renaming. We walk
 * each path in the graph keeping track of every live definition. Thus when we see a
 * variable that shares the VN with a live definition, we'd replace this variable with
 * the variable in the live definition.
 *
 * We do this to be in conventional SSA form. This can very well be changed later.
 *
 * For example, on some path in the graph:
 *    a0 = x0
 *    :            <- other blocks
 *    :
 *    a1 = y0
 *    :
 *    :            <- other blocks
 *    b0 = x0, we cannot substitute x0 with a0, because currently our backend doesn't
 * treat lclNum and ssaNum together as a variable, but just looks at lclNum. If we
 * substituted x0 with a0, then we'd be in general SSA form.
 *
 */
void Compiler::optVnCopyProp()
{
#ifdef DEBUG
    if (verbose)
    {
        printf("*************** In optVnCopyProp()\n");
    }
#endif

    if (fgSsaPassesCompleted == 0)
    {
        return;
    }

    CompAllocator allocator(getAllocator(CMK_CopyProp));

    // Compute the domTree to use.
    BlkToBlkVectorMap* domTree = new (allocator) BlkToBlkVectorMap(allocator);
    domTree->Reallocate(fgBBcount * 3 / 2); // Prime the allocation
    SsaBuilder::ComputeDominators(this, domTree);

    struct BlockWork
    {
        BasicBlock* m_blk;
        bool        m_processed;

        BlockWork(BasicBlock* blk, bool processed = false) : m_blk(blk), m_processed(processed)
        {
        }
    };
    typedef jitstd::vector<BlockWork> BlockWorkStack;

    VarSetOps::AssignNoCopy(this, compCurLife, VarSetOps::MakeEmpty(this));
    VarSetOps::AssignNoCopy(this, optCopyPropKillSet, VarSetOps::MakeEmpty(this));

    // The map from lclNum to its recently live definitions as a stack.
    LclNumToGenTreePtrStack curSsaName(allocator);

    BlockWorkStack* worklist = new (allocator) BlockWorkStack(allocator);

    worklist->push_back(BlockWork(fgFirstBB));
    while (!worklist->empty())
    {
        BlockWork work = worklist->back();
        worklist->pop_back();

        BasicBlock* block = work.m_blk;
        if (work.m_processed)
        {
            // Pop all the live definitions for this block.
            optBlockCopyPropPopStacks(block, &curSsaName);
            continue;
        }

        // Generate copy assertions in this block, and keeping curSsaName variable up to date.
        worklist->push_back(BlockWork(block, true));

        optBlockCopyProp(block, &curSsaName);

        // Add dom children to work on.
        BlkVector* domChildren = domTree->LookupPointer(block);
        if (domChildren != nullptr)
        {
            for (BasicBlock* child : *domChildren)
            {
                worklist->push_back(BlockWork(child));
            }
        }
    }

    // Tracked variable count increases after CopyProp, so don't keep a shorter array around.
    // Destroy (release) the varset.
    VarSetOps::AssignNoCopy(this, compCurLife, VarSetOps::UninitVal());
}
