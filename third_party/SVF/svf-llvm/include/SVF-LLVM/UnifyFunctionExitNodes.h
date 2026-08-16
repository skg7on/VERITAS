//===- UnifyFunctionExitNodes.h - SVF-local port of removed LLVM pass -----===//
//
// LLVM upstream removed llvm::UnifyFunctionExitNodesPass in commit
// 30abd9ec2b8d ("[UnifyFunctionExitNodes] Remove the pass", PR #205519).
// SVF's prePassSchedule still relies on the transformation to normalize each
// function to a single return and a single unreachable block before points-to
// analysis. This header provides a standalone port of the deleted logic that
// depends only on stable LLVM IR APIs.
//
// Original source (pre-removal):
//   llvm/lib/Transforms/Utils/UnifyFunctionExitNodes.cpp
// LICENSE: Apache-2.0 WITH LLVM-exception (unchanged from upstream).
//
//===----------------------------------------------------------------------===//

#ifndef SVF_LLVM_UNIFY_FUNCTION_EXIT_NODES_H
#define SVF_LLVM_UNIFY_FUNCTION_EXIT_NODES_H

#include <vector>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

namespace SVF
{

inline bool unifyUnreachableBlocks(llvm::Function& F)
{
    std::vector<llvm::BasicBlock*> UnreachableBlocks;
    for (llvm::BasicBlock& BB : F)
        if (llvm::isa<llvm::UnreachableInst>(BB.getTerminator()))
            UnreachableBlocks.push_back(&BB);

    if (UnreachableBlocks.size() <= 1)
        return false;

    llvm::BasicBlock* UnreachableBlock =
        llvm::BasicBlock::Create(F.getContext(), "UnifiedUnreachableBlock", &F);
    new llvm::UnreachableInst(F.getContext(), UnreachableBlock);

    for (llvm::BasicBlock* BB : UnreachableBlocks)
    {
        BB->back().eraseFromParent();
        llvm::BranchInst::Create(UnreachableBlock, BB);
    }
    return true;
}

inline bool unifyReturnBlocks(llvm::Function& F)
{
    std::vector<llvm::BasicBlock*> ReturningBlocks;
    for (llvm::BasicBlock& BB : F)
        if (llvm::isa<llvm::ReturnInst>(BB.getTerminator()))
            ReturningBlocks.push_back(&BB);

    if (ReturningBlocks.size() <= 1)
        return false;

    llvm::BasicBlock* NewRetBlock =
        llvm::BasicBlock::Create(F.getContext(), "UnifiedReturnBlock", &F);

    llvm::PHINode* PN = nullptr;
    if (F.getReturnType()->isVoidTy())
    {
        llvm::ReturnInst::Create(F.getContext(), nullptr, NewRetBlock);
    }
    else
    {
        PN = llvm::PHINode::Create(F.getReturnType(), ReturningBlocks.size(),
                                   "UnifiedRetVal");
        PN->insertInto(NewRetBlock, NewRetBlock->end());
        llvm::ReturnInst::Create(F.getContext(), PN, NewRetBlock);
    }

    for (llvm::BasicBlock* BB : ReturningBlocks)
    {
        if (PN)
            PN->addIncoming(BB->getTerminator()->getOperand(0), BB);
        BB->back().eraseFromParent();
        llvm::BranchInst::Create(NewRetBlock, BB);
    }
    return true;
}

inline bool unifyFunctionExitNodes(llvm::Function& F)
{
    bool Changed = false;
    Changed |= unifyUnreachableBlocks(F);
    Changed |= unifyReturnBlocks(F);
    return Changed;
}

} // namespace SVF

#endif // SVF_LLVM_UNIFY_FUNCTION_EXIT_NODES_H
