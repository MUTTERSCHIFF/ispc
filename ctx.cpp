/*
  Copyright (c) 2010-2011, Intel Corporation
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.


   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  
*/

/** @file ctx.cpp
    @brief Implementation of the FunctionEmitContext class
*/

#include "ctx.h"
#include "util.h"
#include "func.h"
#include "llvmutil.h"
#include "type.h"
#include "stmt.h"
#include "expr.h"
#include "module.h"
#include "sym.h"
#include <map>
#include <llvm/DerivedTypes.h>
#include <llvm/Instructions.h>
#include <llvm/Support/Dwarf.h>
#include <llvm/Metadata.h>
#include <llvm/Module.h>

/** This is a small utility structure that records information related to one
    level of nested control flow.  It's mostly used in correctly restoring
    the mask and other state as we exit control flow nesting levels. 
*/
struct CFInfo {
    /** Returns a new instance of the structure that represents entering an
        'if' statement */
    static CFInfo *GetIf(bool isUniform, llvm::Value *savedMask);

    /** Returns a new instance of the structure that represents entering a
        loop. */
    static CFInfo *GetLoop(bool isUniform, llvm::BasicBlock *breakTarget,
                           llvm::BasicBlock *continueTarget, 
                           llvm::Value *savedBreakLanesPtr,
                           llvm::Value *savedContinueLanesPtr,
                           llvm::Value *savedMask, llvm::Value *savedLoopMask);

    bool IsIf() { return type == If; }
    bool IsLoop() { return type == Loop; }
    bool IsVaryingType() { return !isUniform; }
    bool IsUniform() { return isUniform; }

    enum CFType { If, Loop };
    CFType type;
    bool isUniform;
    llvm::BasicBlock *savedBreakTarget, *savedContinueTarget;
    llvm::Value *savedBreakLanesPtr, *savedContinueLanesPtr;
    llvm::Value *savedMask, *savedLoopMask;

private:
    CFInfo(CFType t, bool uniformIf, llvm::Value *sm) {
        assert(t == If);
        type = t;
        isUniform = uniformIf;
        savedBreakTarget = savedContinueTarget = NULL;
        savedBreakLanesPtr = savedContinueLanesPtr = NULL;
        savedMask = savedLoopMask = sm;
    }
    CFInfo(CFType t, bool iu, llvm::BasicBlock *bt, llvm::BasicBlock *ct,
           llvm::Value *sb, llvm::Value *sc, llvm::Value *sm,
           llvm::Value *lm) {
        assert(t == Loop);
        type = t;
        isUniform = iu;
        savedBreakTarget = bt;
        savedContinueTarget = ct;
        savedBreakLanesPtr = sb;
        savedContinueLanesPtr = sc;
        savedMask = sm;
        savedLoopMask = lm;
    }
};


CFInfo *
CFInfo::GetIf(bool isUniform, llvm::Value *savedMask) {
    return new CFInfo(If, isUniform, savedMask);
}


CFInfo *
CFInfo::GetLoop(bool isUniform, llvm::BasicBlock *breakTarget,
                llvm::BasicBlock *continueTarget, 
                llvm::Value *savedBreakLanesPtr,
                llvm::Value *savedContinueLanesPtr,
                llvm::Value *savedMask, llvm::Value *savedLoopMask) {
    return new CFInfo(Loop, isUniform, breakTarget, continueTarget,
                      savedBreakLanesPtr, savedContinueLanesPtr,
                      savedMask, savedLoopMask);
}

///////////////////////////////////////////////////////////////////////////

FunctionEmitContext::FunctionEmitContext(Function *func, Symbol *funSym,
                                         llvm::Function *llvmFunction,
                                         SourcePos firstStmtPos) {
    function = func;

    /* Create a new basic block to store all of the allocas */
    allocaBlock = llvm::BasicBlock::Create(*g->ctx, "allocas", llvmFunction, 0);
    bblock = llvm::BasicBlock::Create(*g->ctx, "entry", llvmFunction, 0);
    /* But jump from it immediately into the real entry block */
    llvm::BranchInst::Create(bblock, allocaBlock);

    funcStartPos = funSym->pos;

    internalMaskPointer = AllocaInst(LLVMTypes::MaskType, "internal_mask_memory");
    StoreInst(LLVMMaskAllOn, internalMaskPointer);
    functionMaskValue = LLVMMaskAllOn;
    fullMaskPointer = NULL;

    loopMask = NULL;
    breakLanesPtr = continueLanesPtr = NULL;
    breakTarget = continueTarget = NULL;

    returnedLanesPtr = AllocaInst(LLVMTypes::MaskType, "returned_lanes_memory");
    StoreInst(LLVMMaskAllOff, returnedLanesPtr);

    launchedTasks = false;
    launchGroupHandlePtr = AllocaInst(LLVMTypes::VoidPointerType, "launch_group_handle");
    StoreInst(llvm::Constant::getNullValue(LLVMTypes::VoidPointerType), 
              launchGroupHandlePtr);

    const Type *returnType = function->GetReturnType();
    if (!returnType || returnType == AtomicType::Void)
        returnValuePtr = NULL;
    else {
        LLVM_TYPE_CONST llvm::Type *ftype = returnType->LLVMType(g->ctx);
        returnValuePtr = AllocaInst(ftype, "return_value_memory");
    }

    if (m->diBuilder) {
        /* If debugging is enabled, tell the debug information emission
           code about this new function */
        diFile = funcStartPos.GetDIFile();
        llvm::DIType retType = function->GetReturnType()->GetDIType(diFile);
        int flags = llvm::DIDescriptor::FlagPrototyped; // ??
        diFunction = m->diBuilder->createFunction(diFile, /* scope */
                                                  llvmFunction->getName(), // mangled
                                                  funSym->name,
                                                  diFile,
                                                  funcStartPos.first_line,
                                                  retType,
                                                  funSym->storageClass == SC_STATIC,
                                                  true, /* is definition */
                                                  flags,
                                                  g->opt.level > 0,
                                                  llvmFunction);
        /* And start a scope representing the initial function scope */
        StartScope();

        llvm::DIFile file = funcStartPos.GetDIFile();
        Symbol *programIndexSymbol = m->symbolTable->LookupVariable("programIndex");
        assert(programIndexSymbol && programIndexSymbol->storagePtr);
        m->diBuilder->createGlobalVariable(programIndexSymbol->name, 
                                           file,
                                           funcStartPos.first_line,
                                           programIndexSymbol->type->GetDIType(file),
                                           true /* static */,
                                           programIndexSymbol->storagePtr);

        Symbol *programCountSymbol = m->symbolTable->LookupVariable("programCount");
        assert(programCountSymbol);
        m->diBuilder->createGlobalVariable(programCountSymbol->name, 
                                           file,
                                           funcStartPos.first_line,
                                           programCountSymbol->type->GetDIType(file),
                                           true /* static */,
                                           programCountSymbol->storagePtr);
    }
}


FunctionEmitContext::~FunctionEmitContext() {
    assert(controlFlowInfo.size() == 0);
    assert(debugScopes.size() == (m->diBuilder ? 1 : 0));
}


const Function *
FunctionEmitContext::GetFunction() const {
    return function;
}


llvm::BasicBlock *
FunctionEmitContext::GetCurrentBasicBlock() {
    return bblock;
}


void
FunctionEmitContext::SetCurrentBasicBlock(llvm::BasicBlock *bb) {
    bblock = bb;
}


llvm::Value *
FunctionEmitContext::GetFunctionMask() {
    return functionMaskValue;
}


llvm::Value *
FunctionEmitContext::GetInternalMask() {
    if (VaryingCFDepth() == 0)
        return LLVMMaskAllOn;
    else
        return LoadInst(internalMaskPointer, NULL, NULL, "load_mask");
}


llvm::Value *
FunctionEmitContext::GetFullMask() {
    llvm::Value *internalMask = GetInternalMask();
    if (internalMask == LLVMMaskAllOn && functionMaskValue == LLVMMaskAllOn)
        return LLVMMaskAllOn;
    else
        return BinaryOperator(llvm::Instruction::And, GetInternalMask(), 
                              functionMaskValue, "internal_mask&function_mask");
}


void
FunctionEmitContext::SetMaskPointer(llvm::Value *p) {
    fullMaskPointer = p;
}


void
FunctionEmitContext::SetFunctionMask(llvm::Value *value) {
    functionMaskValue = value;
    StoreInst(GetFullMask(), fullMaskPointer);
}


void
FunctionEmitContext::SetLoopMask(llvm::Value *value) {
    loopMask = value;
}


void
FunctionEmitContext::SetInternalMask(llvm::Value *value) {
    StoreInst(value, internalMaskPointer);
    // kludge so that __mask returns the right value in ispc code.
    StoreInst(GetFullMask(), fullMaskPointer);
}


void
FunctionEmitContext::SetInternalMaskAnd(llvm::Value *oldMask, llvm::Value *test) {
    llvm::Value *mask = BinaryOperator(llvm::Instruction::And, oldMask, 
                                       test, "oldMask&test");
    SetInternalMask(mask);
}


void
FunctionEmitContext::SetInternalMaskAndNot(llvm::Value *oldMask, llvm::Value *test) {
    llvm::Value *notTest = BinaryOperator(llvm::Instruction::Xor, test, LLVMMaskAllOn,
                                          "~test");
    llvm::Value *mask = BinaryOperator(llvm::Instruction::And, oldMask, notTest,
                                       "oldMask&~test");
    SetInternalMask(mask);
}


void
FunctionEmitContext::BranchIfMaskAny(llvm::BasicBlock *btrue, llvm::BasicBlock *bfalse) {
    assert(bblock != NULL);
    llvm::Value *any = Any(GetFullMask());
    BranchInst(btrue, bfalse, any);
    // It's illegal to add any additional instructions to the basic block
    // now that it's terminated, so set bblock to NULL to be safe
    bblock = NULL;
}


void
FunctionEmitContext::BranchIfMaskAll(llvm::BasicBlock *btrue, llvm::BasicBlock *bfalse) {
    assert(bblock != NULL);
    llvm::Value *all = All(GetFullMask());
    BranchInst(btrue, bfalse, all);
    // It's illegal to add any additional instructions to the basic block
    // now that it's terminated, so set bblock to NULL to be safe
    bblock = NULL;
}


void
FunctionEmitContext::BranchIfMaskNone(llvm::BasicBlock *btrue, llvm::BasicBlock *bfalse) {
    assert(bblock != NULL);
    // switch sense of true/false bblocks
    BranchIfMaskAny(bfalse, btrue);
    // It's illegal to add any additional instructions to the basic block
    // now that it's terminated, so set bblock to NULL to be safe
    bblock = NULL;
}


void
FunctionEmitContext::StartUniformIf() {
    controlFlowInfo.push_back(CFInfo::GetIf(true, GetInternalMask()));
}


void
FunctionEmitContext::StartVaryingIf(llvm::Value *oldMask) {
    controlFlowInfo.push_back(CFInfo::GetIf(false, oldMask));
}


void
FunctionEmitContext::EndIf() {
    // Make sure we match up with a Start{Uniform,Varying}If().
    assert(controlFlowInfo.size() > 0 && controlFlowInfo.back()->IsIf());
    CFInfo *ci = controlFlowInfo.back();
    controlFlowInfo.pop_back();

    // 'uniform' ifs don't change the mask so we only need to restore the
    // mask going into the if for 'varying' if statements
    if (!ci->IsUniform() && bblock != NULL) {
        // We can't just restore the mask as it was going into the 'if'
        // statement.  First we have to take into account any program
        // instances that have executed 'return' statements; the restored
        // mask must be off for those lanes.
        restoreMaskGivenReturns(ci->savedMask);

        // If the 'if' statement is inside a loop with a 'varying'
        // consdition, we also need to account for any break or continue
        // statements that executed inside the 'if' statmeent; we also must
        // leave the lane masks for the program instances that ran those
        // off after we restore the mask after the 'if'.  The code below
        // ends up being optimized out in the case that there were no break
        // or continue statements (and breakLanesPtr and continueLanesPtr
        // have their initial 'all off' values), so we don't need to check
        // for that here.
        if (breakLanesPtr != NULL) {
            assert(continueLanesPtr != NULL);

            // newMask = (oldMask & ~(breakLanes | continueLanes))
            llvm::Value *oldMask = GetInternalMask();
            llvm::Value *breakLanes = LoadInst(breakLanesPtr, NULL, NULL,
                                               "break_lanes");
            llvm::Value *continueLanes = LoadInst(continueLanesPtr, NULL, NULL, 
                                                  "continue_lanes");
            llvm::Value *breakOrContinueLanes = 
                BinaryOperator(llvm::Instruction::Or, breakLanes, continueLanes,
                               "break|continue_lanes");
            llvm::Value *notBreakOrContinue = NotOperator(breakOrContinueLanes,
                                                          "!(break|continue)_lanes");
            llvm::Value *newMask = 
                BinaryOperator(llvm::Instruction::And, oldMask, notBreakOrContinue, 
                               "new_mask");
            SetInternalMask(newMask);
        }
    }
}


void
FunctionEmitContext::StartLoop(llvm::BasicBlock *bt, llvm::BasicBlock *ct, 
                               bool uniformCF) {
    // Store the current values of various loop-related state so that we
    // can restore it when we exit this loop.
    llvm::Value *oldMask = GetInternalMask();
    controlFlowInfo.push_back(CFInfo::GetLoop(uniformCF, breakTarget, 
                                              continueTarget, breakLanesPtr,
                                              continueLanesPtr, oldMask, loopMask));
    if (uniformCF)
        // If the loop has a uniform condition, we don't need to track
        // which lanes 'break' or 'continue'; all of the running ones go
        // together, so we just jump
        breakLanesPtr = continueLanesPtr = NULL;
    else {
        // For loops with varying conditions, allocate space to store masks
        // that record which lanes have done these
        continueLanesPtr = AllocaInst(LLVMTypes::MaskType, "continue_lanes_memory");
        StoreInst(LLVMMaskAllOff, continueLanesPtr);
        breakLanesPtr = AllocaInst(LLVMTypes::MaskType, "break_lanes_memory");
        StoreInst(LLVMMaskAllOff, breakLanesPtr);
    }

    breakTarget = bt;
    continueTarget = ct;
    loopMask = NULL; // this better be set by the loop!
}


void
FunctionEmitContext::EndLoop() {
    assert(controlFlowInfo.size() && !controlFlowInfo.back()->IsIf());
    CFInfo *ci = controlFlowInfo.back();
    controlFlowInfo.pop_back();

    // Restore the break/continue state information to what it was before
    // we went into this loop.
    breakTarget = ci->savedBreakTarget;
    continueTarget = ci->savedContinueTarget;
    breakLanesPtr = ci->savedBreakLanesPtr;
    continueLanesPtr = ci->savedContinueLanesPtr;
    loopMask = ci->savedLoopMask;

    if (!ci->IsUniform())
        // If the loop had a 'uniform' test, then it didn't make any
        // changes to the mask so there's nothing to restore.  If it had a
        // varying test, we need to restore the mask to what it was going
        // into the loop, but still leaving off any lanes that executed a
        // 'return' statement.
        restoreMaskGivenReturns(ci->savedMask);
}


void
FunctionEmitContext::restoreMaskGivenReturns(llvm::Value *oldMask) {
    if (!bblock)
        return;

    // Restore the mask to the given old mask, but leave off any lanes that
    // executed a return statement.
    // newMask = (oldMask & ~returnedLanes)
    llvm::Value *returnedLanes = LoadInst(returnedLanesPtr, NULL, NULL,
                                          "returned_lanes");
    llvm::Value *notReturned = NotOperator(returnedLanes, "~returned_lanes");
    llvm::Value *newMask = BinaryOperator(llvm::Instruction::And,
                                          oldMask, notReturned, "new_mask");
    SetInternalMask(newMask);
}


void
FunctionEmitContext::Break(bool doCoherenceCheck) {
    if (breakTarget == NULL) {
        Error(currentPos, "\"break\" statement is illegal outside of for/while/do loops.");
        return;
    }

    // If all of the enclosing 'if' tests in the loop have uniform control
    // flow or if we can tell that the mask is all on, then we can just
    // jump to the break location.
    if (ifsInLoopAllUniform() || GetInternalMask() == LLVMMaskAllOn) {
        BranchInst(breakTarget);
        if (ifsInLoopAllUniform() && doCoherenceCheck)
            Warning(currentPos, "Coherent break statement not necessary in fully uniform "
                    "control flow.");
        // Set bblock to NULL since the jump has terminated the basic block
        bblock = NULL;
    }
    else {
        // Otherwise we need to update the mask of the lanes that have
        // executed a 'break' statement:
        // breakLanes = breakLanes | mask
        assert(breakLanesPtr != NULL);
        llvm::Value *mask = GetInternalMask();
        llvm::Value *breakMask = LoadInst(breakLanesPtr, NULL, NULL, 
                                          "break_mask");
        llvm::Value *newMask = BinaryOperator(llvm::Instruction::Or,
                                              mask, breakMask, "mask|break_mask");
        StoreInst(newMask, breakLanesPtr);

        // Set the current mask to be all off, just in case there are any
        // statements in the same scope after the 'break'.  Most of time
        // this will be optimized away since we'll likely end the scope of
        // an 'if' statement and restore the mask then.
        SetInternalMask(LLVMMaskAllOff);

        if (doCoherenceCheck)
            // If the user has indicated that this is a 'coherent' break
            // statement, then check to see if the mask is all off.  If so,
            // we have to conservatively jump to the continueTarget, not
            // the breakTarget, since part of the reason the mask is all
            // off may be due to 'continue' statements that executed in the
            // current loop iteration.  
            // FIXME: if the loop only has break statements and no
            // continues, we can jump to breakTarget in that case.
            jumpIfAllLoopLanesAreDone(continueTarget);
    }
}


void
FunctionEmitContext::Continue(bool doCoherenceCheck) {
    if (!continueTarget) {
        Error(currentPos, "\"continue\" statement illegal outside of for/while/do loops.");
        return;
    }

    if (ifsInLoopAllUniform() || GetInternalMask() == LLVMMaskAllOn) {
        // Similarly to 'break' statements, we can immediately jump to the
        // continue target if we're only in 'uniform' control flow within
        // loop or if we can tell that the mask is all on.
        AddInstrumentationPoint("continue: uniform CF, jumped");
        if (ifsInLoopAllUniform() && doCoherenceCheck)
            Warning(currentPos, "Coherent continue statement not necessary in fully uniform "
                    "control flow.");
        BranchInst(continueTarget);
        bblock = NULL;
    }
    else {
        // Otherwise update the stored value of which lanes have 'continue'd.
        // continueLanes = continueLanes | mask
        assert(continueLanesPtr);
        llvm::Value *mask = GetInternalMask();
        llvm::Value *continueMask = 
            LoadInst(continueLanesPtr, NULL, NULL, "continue_mask");
        llvm::Value *newMask = BinaryOperator(llvm::Instruction::Or,
                                              mask, continueMask, "mask|continueMask");
        StoreInst(newMask, continueLanesPtr);

        // And set the current mask to be all off in case there are any
        // statements in the same scope after the 'continue'
        SetInternalMask(LLVMMaskAllOff);

        if (doCoherenceCheck) 
            // If this is a 'coherent continue' statement, then emit the
            // code to see if all of the lanes are now off due to
            // breaks/continues and jump to the continue target if so.
            jumpIfAllLoopLanesAreDone(continueTarget);
    }
}


/** This function checks to see if all of the 'if' statements (if any)
    between the current scope and the first enclosing loop have 'uniform'
    tests.
 */
bool
FunctionEmitContext::ifsInLoopAllUniform() const {
    assert(controlFlowInfo.size() > 0);
    // Go backwards through controlFlowInfo, since we add new nested scopes
    // to the back.  Stop once we come to the first enclosing loop.
    int i = controlFlowInfo.size() - 1;
    while (i >= 0 && controlFlowInfo[i]->type != CFInfo::Loop) {
        if (controlFlowInfo[i]->isUniform == false)
            // Found a scope due to an 'if' statement with a varying test
            return false;
        --i;
    }
    assert(i >= 0); // else we didn't find a loop!
    return true;
}


void
FunctionEmitContext::jumpIfAllLoopLanesAreDone(llvm::BasicBlock *target) {
    // Check to see if (returned lanes | continued lanes | break lanes) is
    // equal to the value of mask at the start of the loop iteration.  If
    // so, everyone is done and we can jump to the given target
    llvm::Value *returned = LoadInst(returnedLanesPtr, NULL, NULL,
                                     "returned_lanes");
    llvm::Value *continued = LoadInst(continueLanesPtr, NULL, NULL,
                                      "continue_lanes");
    llvm::Value *breaked = LoadInst(breakLanesPtr, NULL, NULL,
                                    "break_lanes");
    llvm::Value *returnedOrContinued = BinaryOperator(llvm::Instruction::Or, 
                                                      returned, continued,
                                                      "returned|continued");
    llvm::Value *returnedOrContinuedOrBreaked = 
        BinaryOperator(llvm::Instruction::Or, returnedOrContinued,
                       breaked, "returned|continued");

    // Do we match the mask at loop entry?
    llvm::Value *allRCB = MasksAllEqual(returnedOrContinuedOrBreaked, loopMask);
    llvm::BasicBlock *bAll = CreateBasicBlock("all_continued_or_breaked");
    llvm::BasicBlock *bNotAll = CreateBasicBlock("not_all_continued_or_breaked");
    BranchInst(bAll, bNotAll, allRCB);

    // If so, have an extra basic block along the way to add
    // instrumentation, if the user asked for it.
    bblock = bAll;
    AddInstrumentationPoint("break/continue: all dynamically went");
    BranchInst(target);

    // And set the current basic block to a new one for future instructions
    // for the path where we weren't able to jump
    bblock = bNotAll;
    AddInstrumentationPoint("break/continue: not all went");
}


void
FunctionEmitContext::RestoreContinuedLanes() {
    if (continueLanesPtr == NULL)
        return;

    // mask = mask & continueFlags
    llvm::Value *mask = GetInternalMask();
    llvm::Value *continueMask = LoadInst(continueLanesPtr, NULL, NULL,
                                         "continue_mask");
    llvm::Value *orMask = BinaryOperator(llvm::Instruction::Or,
                                         mask, continueMask, "mask|continue_mask");
    SetInternalMask(orMask);

    // continueLanes = 0
    StoreInst(LLVMMaskAllOff, continueLanesPtr);
}


int
FunctionEmitContext::VaryingCFDepth() const { 
    int sum = 0;
    for (unsigned int i = 0; i < controlFlowInfo.size(); ++i)
        if (controlFlowInfo[i]->IsVaryingType())
            ++sum;
    return sum;
}


void
FunctionEmitContext::CurrentLanesReturned(Expr *expr, bool doCoherenceCheck) {
    const Type *returnType = function->GetReturnType();
    if (returnType == AtomicType::Void) {
        if (expr != NULL)
            Error(expr->pos, "Can't return non-void type \"%s\" from void function.",
                  expr->GetType()->GetString().c_str());
    }
    else {
        if (expr == NULL) {
            Error(funcStartPos, "Must provide return value for return "
                  "statement for non-void function.");
            return;
        }
        
        expr = TypeConvertExpr(expr, returnType, "return statement");
        if (expr != NULL) {
            llvm::Value *retVal = expr->GetValue(this);
            if (retVal != NULL)
                // Use a masked store to store the value of the expression
                // in the return value memory; this preserves the return
                // values from other lanes that may have executed return
                // statements previously.
                StoreInst(retVal, returnValuePtr, GetInternalMask(), returnType);
        }
    }

    if (VaryingCFDepth() == 0) {
        // If there is only uniform control flow between us and the
        // function entry, then it's guaranteed that all lanes are running,
        // so we can just emit a true return instruction
        AddInstrumentationPoint("return: uniform control flow");
        ReturnInst();
    }
    else {
        // Otherwise we update the returnedLanes value by ANDing it with
        // the current lane mask.
        llvm::Value *oldReturnedLanes = LoadInst(returnedLanesPtr, NULL, NULL,
                                                 "old_returned_lanes");
        llvm::Value *newReturnedLanes = 
            BinaryOperator(llvm::Instruction::Or, oldReturnedLanes, 
                           GetInternalMask(), "old_mask|returned_lanes");
        
        // For 'coherent' return statements, emit code to check if all
        // lanes have returned
        if (doCoherenceCheck) {
            // if newReturnedLanes == functionMaskValue, get out of here!
            llvm::Value *cmp = MasksAllEqual(functionMaskValue, 
                                             newReturnedLanes);
            llvm::BasicBlock *bDoReturn = CreateBasicBlock("do_return");
            llvm::BasicBlock *bNoReturn = CreateBasicBlock("no_return");
            BranchInst(bDoReturn, bNoReturn, cmp);

            bblock = bDoReturn;
            AddInstrumentationPoint("return: all lanes have returned");
            ReturnInst();

            bblock = bNoReturn;
        }
        // Otherwise update returnedLanesPtr and turn off all of the lanes
        // in the current mask so that any subsequent statements in the
        // same scope after the return have no effect
        StoreInst(newReturnedLanes, returnedLanesPtr);
        AddInstrumentationPoint("return: some but not all lanes have returned");
        SetInternalMask(LLVMMaskAllOff);
    }
}


llvm::Value *
FunctionEmitContext::Any(llvm::Value *mask) {
    llvm::Value *mmval = LaneMask(mask);
    return CmpInst(llvm::Instruction::ICmp, llvm::CmpInst::ICMP_NE, mmval,
                   LLVMInt32(0), "any_mm_cmp");
}


llvm::Value *
FunctionEmitContext::All(llvm::Value *mask) {
    llvm::Value *mmval = LaneMask(mask);
    return CmpInst(llvm::Instruction::ICmp, llvm::CmpInst::ICMP_EQ, mmval,
                   LLVMInt32((1<<g->target.vectorWidth)-1), "all_mm_cmp");
}


llvm::Value *
FunctionEmitContext::LaneMask(llvm::Value *v) {
    // Call the target-dependent movmsk function to turn the vector mask
    // into an i32 value
    std::vector<Symbol *> *mm = m->symbolTable->LookupFunction("__movmsk");
    // There should be one with signed int signature, one unsigned int.
    assert(mm && mm->size() == 2); 
    llvm::Function *fmm = (*mm)[0]->function;
    return CallInst(fmm, AtomicType::UniformInt32, v, "val_movmsk");
}


llvm::Value *
FunctionEmitContext::MasksAllEqual(llvm::Value *v1, llvm::Value *v2) {
#if 0
    // Compare the two masks to get a vector of i1s
    llvm::Value *cmp = CmpInst(llvm::Instruction::ICmp, llvm::CmpInst::ICMP_EQ,
                               v1, v2, "v1==v2");
    // Turn that into a bool vector type (often i32s)
    cmp = I1VecToBoolVec(cmp);
    // And see if it's all on
    return All(cmp);
#else
    llvm::Value *mm1 = LaneMask(v1);
    llvm::Value *mm2 = LaneMask(v2);
    return CmpInst(llvm::Instruction::ICmp, llvm::CmpInst::ICMP_EQ, mm1, mm2,
                   "v1==v2");
#endif
}


llvm::Value *
FunctionEmitContext::GetStringPtr(const std::string &str) {
    llvm::Constant *lstr = llvm::ConstantArray::get(*g->ctx, str);
    llvm::GlobalValue::LinkageTypes linkage = llvm::GlobalValue::InternalLinkage;
    llvm::Value *lstrPtr = new llvm::GlobalVariable(*m->module, lstr->getType(),
                                                    true /*isConst*/, 
                                                    linkage, lstr, "__str");
    return new llvm::BitCastInst(lstrPtr, LLVMTypes::VoidPointerType, 
                                 "str_void_ptr", bblock);
}


llvm::BasicBlock *
FunctionEmitContext::CreateBasicBlock(const char *name) {
    llvm::Function *function = bblock->getParent();
    return llvm::BasicBlock::Create(*g->ctx, name, function);
}


llvm::Value *
FunctionEmitContext::I1VecToBoolVec(llvm::Value *b) {
    LLVM_TYPE_CONST llvm::ArrayType *at = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(b->getType());
    if (at) {
        // If we're given an array of vectors of i1s, then do the
        // conversion for each of the elements
        LLVM_TYPE_CONST llvm::Type *boolArrayType = 
            llvm::ArrayType::get(LLVMTypes::BoolVectorType, at->getNumElements());
        llvm::Value *ret = llvm::UndefValue::get(boolArrayType);

        for (unsigned int i = 0; i < at->getNumElements(); ++i) {
            llvm::Value *elt = ExtractInst(b, i);
            llvm::Value *sext = SExtInst(elt, LLVMTypes::BoolVectorType, 
                                         "val_to_boolvec32");
            ret = InsertInst(ret, sext, i);
        }
        return ret;
    }
    else
        return SExtInst(b, LLVMTypes::BoolVectorType, "val_to_boolvec32");
}


static llvm::Value *
lGetStringAsValue(llvm::BasicBlock *bblock, const char *s) {
    llvm::Constant *sConstant = llvm::ConstantArray::get(*g->ctx, s);
    llvm::Value *sPtr = new llvm::GlobalVariable(*m->module, sConstant->getType(), 
                                                 true /* const */,
                                                 llvm::GlobalValue::InternalLinkage,
                                                 sConstant, s);
    llvm::Value *indices[2] = { LLVMInt32(0), LLVMInt32(0) };
#if defined(LLVM_3_0) || defined(LLVM_3_0svn) || defined(LLVM_3_1svn)
    llvm::ArrayRef<llvm::Value *> arrayRef(&indices[0], &indices[2]);
    return llvm::GetElementPtrInst::Create(sPtr, arrayRef, "sptr", bblock);
#else
    return llvm::GetElementPtrInst::Create(sPtr, &indices[0], &indices[2],
                                           "sptr", bblock);
#endif
}


void
FunctionEmitContext::AddInstrumentationPoint(const char *note) {
    assert(note != NULL);
    if (!g->emitInstrumentation)
        return;

    std::vector<llvm::Value *> args;
    // arg 1: filename as string
    args.push_back(lGetStringAsValue(bblock, currentPos.name));
    // arg 2: provided note
    args.push_back(lGetStringAsValue(bblock, note));
    // arg 3: line number
    args.push_back(LLVMInt32(currentPos.first_line));
    // arg 4: current mask, movmsk'ed down to an int32
    args.push_back(LaneMask(GetFullMask()));

    llvm::Function *finst = m->module->getFunction("ISPCInstrument");
    CallInst(finst, AtomicType::Void, args, "");
}


void
FunctionEmitContext::SetDebugPos(SourcePos pos) { 
    currentPos = pos; 
}


SourcePos
FunctionEmitContext::GetDebugPos() const {
    return currentPos;
}


void
FunctionEmitContext::AddDebugPos(llvm::Value *value, const SourcePos *pos, 
                                 llvm::DIScope *scope) {
    llvm::Instruction *inst = llvm::dyn_cast<llvm::Instruction>(value);
    if (inst != NULL && m->diBuilder) {
        SourcePos p = pos ? *pos : currentPos;
        if (p.first_line != 0)
            // If first_line == 0, then we're in the middle of setting up
            // the standard library or the like; don't add debug positions
            // for those functions
            inst->setDebugLoc(llvm::DebugLoc::get(p.first_line, p.first_column, 
                                                  scope ? *scope : GetDIScope()));
    }
}


void
FunctionEmitContext::StartScope() {
    if (m->diBuilder != NULL) {
        llvm::DIScope parentScope;
        if (debugScopes.size() > 0)
            parentScope = debugScopes.back();
        else
            parentScope = diFunction;

        llvm::DILexicalBlock lexicalBlock = 
            m->diBuilder->createLexicalBlock(parentScope, diFile,
                                             currentPos.first_line,
                                             currentPos.first_column);
        debugScopes.push_back(lexicalBlock);
    }
}


void
FunctionEmitContext::EndScope() {
    if (m->diBuilder != NULL) {
        assert(debugScopes.size() > 0);
        debugScopes.pop_back();
    }
}


llvm::DIScope 
FunctionEmitContext::GetDIScope() const {
    assert(debugScopes.size() > 0);
    return debugScopes.back();
}


void
FunctionEmitContext::EmitVariableDebugInfo(Symbol *sym) {
    if (m->diBuilder == NULL)
        return;

    llvm::DIScope scope = GetDIScope();
    llvm::DIVariable var = 
        m->diBuilder->createLocalVariable(llvm::dwarf::DW_TAG_auto_variable,
                                          scope,
                                          sym->name,
                                          sym->pos.GetDIFile(),
                                          sym->pos.first_line,
                                          sym->type->GetDIType(scope),
                                          true /* preserve through opts */);
    llvm::Instruction *declareInst = 
        m->diBuilder->insertDeclare(sym->storagePtr, var, bblock);
    AddDebugPos(declareInst, &sym->pos, &scope);
}


void
FunctionEmitContext::EmitFunctionParameterDebugInfo(Symbol *sym) {
    if (m->diBuilder == NULL)
        return;

    llvm::DIScope scope = diFunction;
    llvm::DIVariable var = 
        m->diBuilder->createLocalVariable(llvm::dwarf::DW_TAG_arg_variable,
                                          scope,
                                          sym->name,
                                          sym->pos.GetDIFile(),
                                          sym->pos.first_line,
                                          sym->type->GetDIType(scope),
                                          true /* preserve through opts */);
    llvm::Instruction *declareInst = 
        m->diBuilder->insertDeclare(sym->storagePtr, var, bblock);
    AddDebugPos(declareInst, &sym->pos, &scope);
}


/** If the given type is an array of vector types, then it's the
    representation of an ispc VectorType with varying elements.  If it is
    one of these, return the array size (i.e. the VectorType's size).
    Otherwise return zero.
 */
static int
lArrayVectorWidth(LLVM_TYPE_CONST llvm::Type *t) {
    LLVM_TYPE_CONST llvm::ArrayType *arrayType = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(t);
    if (arrayType == NULL)
        return 0;

    // We shouldn't be seeing arrays of anything but vectors or pointers
    // (for == and !=) being passed to things like
    // FunctionEmitContext::BinaryOperator() as operands
    LLVM_TYPE_CONST llvm::VectorType *vectorElementType = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::VectorType>(arrayType->getElementType());
    LLVM_TYPE_CONST llvm::PointerType *pointerElementType = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::PointerType>(arrayType->getElementType());
    assert((vectorElementType != NULL &&
            (int)vectorElementType->getNumElements() == g->target.vectorWidth) ||
           (pointerElementType != NULL &&
            (int)arrayType->getNumElements() == g->target.vectorWidth));
           
    return (int)arrayType->getNumElements();
}


llvm::Value *
FunctionEmitContext::BinaryOperator(llvm::Instruction::BinaryOps inst, 
                                    llvm::Value *v0, llvm::Value *v1, 
                                    const char *name) {
    if (v0 == NULL || v1 == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    assert(v0->getType() == v1->getType());
    LLVM_TYPE_CONST llvm::Type *type = v0->getType();
    int arraySize = lArrayVectorWidth(type);
    if (arraySize == 0) {
        llvm::Instruction *bop = 
            llvm::BinaryOperator::Create(inst, v0, v1, name ? name : "", bblock);
        AddDebugPos(bop);
        return bop;
    }
    else {
        // If this is an ispc VectorType, apply the binary operator to each
        // of the elements of the array (which in turn should be either
        // scalar types or llvm::VectorTypes.)
        llvm::Value *ret = llvm::UndefValue::get(type);
        for (int i = 0; i < arraySize; ++i) {
            llvm::Value *a = ExtractInst(v0, i);
            llvm::Value *b = ExtractInst(v1, i);
            llvm::Value *op = BinaryOperator(inst, a, b);
            ret = InsertInst(ret, op, i);
        }
        return ret;
    }
}


llvm::Value *
FunctionEmitContext::NotOperator(llvm::Value *v, const char *name) {
    if (v == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    // Similarly to BinaryOperator, do the operation on all the elements of
    // the array if we're given an array type; otherwise just do the
    // regular llvm operation.
    LLVM_TYPE_CONST llvm::Type *type = v->getType();
    int arraySize = lArrayVectorWidth(type);
    if (arraySize == 0) {
        llvm::Instruction *binst = 
            llvm::BinaryOperator::CreateNot(v, name ? name : "not", bblock);
        AddDebugPos(binst);
        return binst;
    }
    else {
        llvm::Value *ret = llvm::UndefValue::get(type);
        for (int i = 0; i < arraySize; ++i) {
            llvm::Value *a = ExtractInst(v, i);
            llvm::Value *op = 
                llvm::BinaryOperator::CreateNot(a, name ? name : "not", bblock);
            AddDebugPos(op);
            ret = InsertInst(ret, op, i);
        }
        return ret;
    }
}


// Given the llvm Type that represents an ispc VectorType (or array of
// pointers), return an equally-shaped type with boolean elements.  (This
// is the type that will be returned from CmpInst with ispc VectorTypes).
static LLVM_TYPE_CONST llvm::Type *
lGetMatchingBoolVectorType(LLVM_TYPE_CONST llvm::Type *type) {
    LLVM_TYPE_CONST llvm::ArrayType *arrayType = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(type);
    assert(arrayType != NULL);

    LLVM_TYPE_CONST llvm::VectorType *vectorElementType = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::VectorType>(arrayType->getElementType());
    if (vectorElementType != NULL) {
        assert((int)vectorElementType->getNumElements() == g->target.vectorWidth);
        LLVM_TYPE_CONST llvm::Type *base = 
            llvm::VectorType::get(LLVMTypes::BoolType, g->target.vectorWidth);
        return llvm::ArrayType::get(base, arrayType->getNumElements());
    }
    else {
        LLVM_TYPE_CONST llvm::PointerType *pointerElementType = 
            llvm::dyn_cast<LLVM_TYPE_CONST llvm::PointerType>(arrayType->getElementType());
        assert(pointerElementType != NULL);
        assert((int)arrayType->getNumElements() == g->target.vectorWidth);
        return llvm::VectorType::get(LLVMTypes::BoolType, g->target.vectorWidth);
    }
}


llvm::Value *
FunctionEmitContext::CmpInst(llvm::Instruction::OtherOps inst, 
                             llvm::CmpInst::Predicate pred,
                             llvm::Value *v0, llvm::Value *v1, 
                             const char *name) {
    if (v0 == NULL || v1 == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    assert(v0->getType() == v1->getType());
    LLVM_TYPE_CONST llvm::Type *type = v0->getType();
    int arraySize = lArrayVectorWidth(type);
    if (arraySize == 0) {
        llvm::Instruction *ci = 
            llvm::CmpInst::Create(inst, pred, v0, v1, name ? name : "cmp", 
                                  bblock);
        AddDebugPos(ci);
        return ci;
    }
    else {
        LLVM_TYPE_CONST llvm::Type *boolType = lGetMatchingBoolVectorType(type);
        llvm::Value *ret = llvm::UndefValue::get(boolType);
        for (int i = 0; i < arraySize; ++i) {
            llvm::Value *a = ExtractInst(v0, i);
            llvm::Value *b = ExtractInst(v1, i);
            llvm::Value *op = CmpInst(inst, pred, a, b, name);
            ret = InsertInst(ret, op, i);
        }
        return ret;
    }
}


llvm::Value *
FunctionEmitContext::SmearScalar(llvm::Value *value, const char *name) {
    if (value == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    llvm::Value *ret = NULL;
    LLVM_TYPE_CONST llvm::Type *eltType = value->getType();
    LLVM_TYPE_CONST llvm::PointerType *pt = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::PointerType>(eltType);
    if (pt != NULL)
        ret = llvm::UndefValue::get(llvm::ArrayType::get(eltType,
                                                         g->target.vectorWidth));
    else
        ret = llvm::UndefValue::get(llvm::VectorType::get(eltType,
                                                          g->target.vectorWidth));

    for (int i = 0; i < g->target.vectorWidth; ++i) {
        llvm::Twine n = llvm::Twine("smear.") + llvm::Twine(name ? name : "") + 
            llvm::Twine(i);
        ret = InsertInst(ret, value, i, n.str().c_str());
    }
    return ret;
}
                                    

llvm::Value *
FunctionEmitContext::BitCastInst(llvm::Value *value, LLVM_TYPE_CONST llvm::Type *type, 
                                 const char *name) {
    if (value == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    LLVM_TYPE_CONST llvm::Type *valType = value->getType();
    LLVM_TYPE_CONST llvm::ArrayType *at = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(valType);
    if (at && llvm::isa<LLVM_TYPE_CONST llvm::PointerType>(at->getElementType())) {
        // If we're bitcasting an array of pointers, we have a varying
        // lvalue; apply the corresponding bitcast to each of the
        // individual pointers and return the result array.
        assert((int)at->getNumElements() == g->target.vectorWidth);

        llvm::Value *ret = 
            llvm::UndefValue::get(llvm::ArrayType::get(type, g->target.vectorWidth));
        for (int i = 0; i < g->target.vectorWidth; ++i) {
            llvm::Value *elt = ExtractInst(value, i);
            llvm::Value *bc = BitCastInst(elt, type, name);
            ret = InsertInst(ret, bc, i);
        }
        return ret;
    }
    else {
        llvm::Instruction *inst = 
            new llvm::BitCastInst(value, type, name ? name : "bitcast", bblock);
        AddDebugPos(inst);
        return inst;
    }
}


llvm::Value *
FunctionEmitContext::PtrToIntInst(llvm::Value *value, LLVM_TYPE_CONST llvm::Type *type,
                                  const char *name) {
    if (value == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    LLVM_TYPE_CONST llvm::Type *valType = value->getType();
    LLVM_TYPE_CONST llvm::ArrayType *at = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(valType);
    if (at && llvm::isa<LLVM_TYPE_CONST llvm::PointerType>(at->getElementType())) {
        // varying lvalue -> apply ptr to int to the individual pointers
        assert((int)at->getNumElements() == g->target.vectorWidth);

        llvm::Value *ret = 
            llvm::UndefValue::get(llvm::ArrayType::get(type, g->target.vectorWidth));
        for (int i = 0; i < g->target.vectorWidth; ++i) {
            llvm::Value *elt = ExtractInst(value, i);
            llvm::Value *p2i = PtrToIntInst(elt, type, name);
            ret = InsertInst(ret, p2i, i);
        }
        return ret;
    }
    else {
        llvm::Instruction *inst = 
            new llvm::PtrToIntInst(value, type, name ? name : "ptr2int", bblock);
        AddDebugPos(inst);
        return inst;
    }
}


llvm::Value *
FunctionEmitContext::IntToPtrInst(llvm::Value *value, LLVM_TYPE_CONST llvm::Type *type,
                                  const char *name) {
    if (value == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    LLVM_TYPE_CONST llvm::Type *valType = value->getType();
    LLVM_TYPE_CONST llvm::ArrayType *at = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(valType);
    if (at != NULL) {
        // varying lvalue -> apply int to ptr to the individual pointers
        assert((int)at->getNumElements() == g->target.vectorWidth);

        llvm::Value *ret = 
            llvm::UndefValue::get(llvm::ArrayType::get(type, g->target.vectorWidth));
        for (int i = 0; i < g->target.vectorWidth; ++i) {
            llvm::Value *elt = ExtractInst(value, i);
            llvm::Value *i2p = IntToPtrInst(elt, type, name);
            ret = InsertInst(ret, i2p, i);
        }
        return ret;
    }
    else {
        llvm::Instruction *inst = 
            new llvm::IntToPtrInst(value, type, name ? name : "int2ptr", bblock);
        AddDebugPos(inst);
        return inst;
    }
}


llvm::Instruction *
FunctionEmitContext::TruncInst(llvm::Value *value, LLVM_TYPE_CONST llvm::Type *type,
                               const char *name) {
    if (value == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    // TODO: we should probably handle the array case as in
    // e.g. BitCastInst(), but we don't currently need that functionality
    llvm::Instruction *inst = 
        new llvm::TruncInst(value, type, name ? name : "trunc", bblock);
    AddDebugPos(inst);
    return inst;
}


llvm::Value *
FunctionEmitContext::ArrayToVectorInst(llvm::Value *array) {
    if (array == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    LLVM_TYPE_CONST llvm::ArrayType *at = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(array->getType());
    assert(at != NULL);

    uint64_t count = at->getNumElements();
    LLVM_TYPE_CONST llvm::VectorType *vt = 
        llvm::VectorType::get(at->getElementType(), count);
    llvm::Value *vec = llvm::UndefValue::get(vt);
    for (uint64_t i = 0; i < count; ++i)
        vec = InsertInst(vec, ExtractInst(array, i), i);
    return vec;
}


llvm::Value *
FunctionEmitContext::VectorToArrayInst(llvm::Value *vector) {
    if (vector == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    LLVM_TYPE_CONST llvm::VectorType *vt = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::VectorType>(vector->getType());
    assert(vt != NULL);

    uint64_t count = vt->getNumElements();
    LLVM_TYPE_CONST llvm::ArrayType *at = 
        llvm::ArrayType::get(vt->getElementType(), count);
    llvm::Value *array = llvm::UndefValue::get(at);
    for (uint64_t i = 0; i < count; ++i)
        array = InsertInst(array, ExtractInst(vector, i), i);
    return array;
}


llvm::Instruction *
FunctionEmitContext::CastInst(llvm::Instruction::CastOps op, llvm::Value *value,
                              LLVM_TYPE_CONST llvm::Type *type, const char *name) {
    if (value == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    // TODO: we should probably handle the array case as in
    // e.g. BitCastInst(), but we don't currently need that functionality
    llvm::Instruction *inst = 
        llvm::CastInst::Create(op, value, type, name ? name : "cast", bblock);
    AddDebugPos(inst);
    return inst;
}


llvm::Instruction *
FunctionEmitContext::FPCastInst(llvm::Value *value, LLVM_TYPE_CONST llvm::Type *type, 
                                const char *name) {
    if (value == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    // TODO: we should probably handle the array case as in
    // e.g. BitCastInst(), but we don't currently need that functionality
    llvm::Instruction *inst = 
        llvm::CastInst::CreateFPCast(value, type, name ? name : "fpcast", bblock);
    AddDebugPos(inst);
    return inst;
}


llvm::Instruction *
FunctionEmitContext::SExtInst(llvm::Value *value, LLVM_TYPE_CONST llvm::Type *type, 
                              const char *name) {
    if (value == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    // TODO: we should probably handle the array case as in
    // e.g. BitCastInst(), but we don't currently need that functionality
    llvm::Instruction *inst = 
        new llvm::SExtInst(value, type, name ? name : "sext", bblock);
    AddDebugPos(inst);
    return inst;
}


llvm::Instruction *
FunctionEmitContext::ZExtInst(llvm::Value *value, LLVM_TYPE_CONST llvm::Type *type, 
                              const char *name) {
    if (value == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    // TODO: we should probably handle the array case as in
    // e.g. BitCastInst(), but we don't currently need that functionality
    llvm::Instruction *inst = 
        new llvm::ZExtInst(value, type, name ? name : "zext", bblock);
    AddDebugPos(inst);
    return inst;
}


llvm::Value *
FunctionEmitContext::GetElementPtrInst(llvm::Value *basePtr, llvm::Value *index0, 
                                       llvm::Value *index1, const char *name) {
    if (basePtr == NULL || index0 == NULL || index1 == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    // FIXME: do we need need to handle the case of the first index being
    // varying?  It's not currently needed...
    assert(!llvm::isa<LLVM_TYPE_CONST llvm::VectorType>(index0->getType()));

    LLVM_TYPE_CONST llvm::Type *basePtrType = basePtr->getType();
    LLVM_TYPE_CONST llvm::ArrayType *baseArrayType = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(basePtrType);
    bool baseIsVaryingTypePointer = (baseArrayType != NULL) && 
        llvm::isa<LLVM_TYPE_CONST llvm::PointerType>(baseArrayType->getElementType());
    bool indexIsVaryingType = 
        llvm::isa<LLVM_TYPE_CONST llvm::VectorType>(index1->getType());

    if (!indexIsVaryingType && !baseIsVaryingTypePointer) {
        // The easy case: both the base pointer and the indices are
        // uniform, so just emit the regular LLVM GEP instruction
        llvm::Value *indices[2] = { index0, index1 };
#if defined(LLVM_3_0) || defined(LLVM_3_0svn) || defined(LLVM_3_1svn)
        llvm::ArrayRef<llvm::Value *> arrayRef(&indices[0], &indices[2]);
        llvm::Instruction *inst = 
            llvm::GetElementPtrInst::Create(basePtr, arrayRef,
                                            name ? name : "gep", bblock);
#else
        llvm::Instruction *inst = 
            llvm::GetElementPtrInst::Create(basePtr, &indices[0], &indices[2], 
                                            name ? name : "gep", bblock);
#endif
        AddDebugPos(inst);
        return inst;
    }
    else {
        // We have a varying pointer and/or indices; emit the appropriate
        // GEP for each of the program instances
        llvm::Value *lret = NULL;
        for (int i = 0; i < g->target.vectorWidth; ++i) {
            // Get the index, either using the same one if it's uniform or
            // the one for this lane if it's varying
            llvm::Value *indexElt;
            if (indexIsVaryingType)
                indexElt = ExtractInst(index1, i, "get_array_index");
            else
                indexElt = index1;

            // Similarly figure out the appropriate base pointer
            llvm::Value *aptr;
            if (baseIsVaryingTypePointer)
                aptr = ExtractInst(basePtr, i, "get_array_index");
            else
                aptr = basePtr;

            // Do the GEP for this lane
            llvm::Value *eltPtr = GetElementPtrInst(aptr, index0, indexElt, name);

            if (lret == NULL) {
                // This is kind of a hack: use the type from the GEP to
                // figure out the return type and the first time through,
                // create an undef value of that type here
                LLVM_TYPE_CONST llvm::PointerType *elementPtrType = 
                    llvm::dyn_cast<LLVM_TYPE_CONST llvm::PointerType>(eltPtr->getType());
                LLVM_TYPE_CONST llvm::Type *elementType = 
                    elementPtrType->getElementType();
                lret = llvm::UndefValue::get(LLVMPointerVectorType(elementType));
            }

            // And insert the result of the GEP into the return value
            lret = InsertInst(lret, eltPtr, i, "elt_ptr_store");
        }
        return lret;
    }
}


llvm::Value *
FunctionEmitContext::GetElementPtrInst(llvm::Value *basePtr, int v0, int v1,
                                       const char *name) {
    return GetElementPtrInst(basePtr, LLVMInt32(v0), LLVMInt32(v1), name);
}
    

llvm::Value *
FunctionEmitContext::LoadInst(llvm::Value *lvalue, llvm::Value *mask,
                              const Type *type, const char *name) {
    if (lvalue == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    if (llvm::isa<LLVM_TYPE_CONST llvm::PointerType>(lvalue->getType())) {
        // If the lvalue is a straight up regular pointer, then just issue
        // a regular load.  First figure out the alignment; in general we
        // can just assume the natural alignment (0 here), but for varying
        // atomic types, we need to make sure that the compiler emits
        // unaligned vector loads, so we specify a reduced alignment here.
        int align = 0;
        const AtomicType *atomicType = dynamic_cast<const AtomicType *>(type);
        if (atomicType != NULL && atomicType->IsVaryingType())
            // We actually just want to align to the vector element
            // alignment, but can't easily get that here, so just tell LLVM
            // it's totally unaligned.  (This shouldn't make any difference
            // vs the proper alignment in practice.)
            align = 1;
        llvm::Instruction *inst = new llvm::LoadInst(lvalue, name ? name : "load",
                                                     false /* not volatile */,
                                                     align, bblock);
        AddDebugPos(inst);
        return inst;
    }
    else {
        // Otherwise we should have a varying lvalue and it's time for a
        // gather.  The "type" parameter only has to be non-NULL for the
        // gather path here (we can't reliably figure out all of the type
        // information we need from the LLVM::Type, so have to carry the
        // ispc type in through this path..
        assert(type != NULL && mask != NULL);
        assert(llvm::isa<LLVM_TYPE_CONST llvm::ArrayType>(lvalue->getType()));
        return gather(lvalue, mask, type, name);
    }
}


llvm::Value *
FunctionEmitContext::gather(llvm::Value *lvalue, llvm::Value *mask,
                            const Type *type, const char *name) {
    // We should have a varying lvalue if we get here...
    assert(llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(lvalue->getType()));

    LLVM_TYPE_CONST llvm::Type *retType = type->LLVMType(g->ctx);

    const StructType *st = dynamic_cast<const StructType *>(type);
    if (st) {
        // If we're gathering structures, do an element-wise gather
        // recursively.
        llvm::Value *retValue = llvm::UndefValue::get(retType);
        for (int i = 0; i < st->GetElementCount(); ++i) {
            llvm::Value *eltPtrs = GetElementPtrInst(lvalue, 0, i);
            // This in turn will be another gather
            llvm::Value *eltValues = 
                LoadInst(eltPtrs, mask, st->GetElementType(i), name);
            retValue = InsertInst(retValue, eltValues, i, "set_value");
        }
        return retValue;
    }

    const VectorType *vt = dynamic_cast<const VectorType *>(type);
    if (vt) {
        // Similarly, if it's a vector type, do a gather for each of the
        // vector elements
        llvm::Value *retValue = llvm::UndefValue::get(retType);
        // FIXME: yuck.  Change lvalues to be pointers to arrays so that
        // the GEP stuff in the loop below ends up computing pointers based
        // on elements in the vectors rather than incorrectly advancing to
        // the next vector...
        LLVM_TYPE_CONST llvm::Type *eltType = 
            vt->GetBaseType()->GetAsUniformType()->LLVMType(g->ctx);
        lvalue = BitCastInst(lvalue, llvm::PointerType::get(llvm::ArrayType::get(eltType, 0), 0));

        for (int i = 0; i < vt->GetElementCount(); ++i) {
            llvm::Value *eltPtrs = GetElementPtrInst(lvalue, 0, i);
            llvm::Value *eltValues = LoadInst(eltPtrs, mask, vt->GetBaseType(), 
                                              name);
            retValue = InsertInst(retValue, eltValues, i, "set_value");
        }
        return retValue;
    }

    const ArrayType *at = dynamic_cast<const ArrayType *>(type);
    if (at) {
        // Arrays are also handled recursively and element-wise
        llvm::Value *retValue = llvm::UndefValue::get(retType);
        for (int i = 0; i < at->GetElementCount(); ++i) {
            llvm::Value *eltPtrs = GetElementPtrInst(lvalue, 0, i);
            llvm::Value *eltValues = LoadInst(eltPtrs, mask, 
                                              at->GetElementType(), name);
            retValue = InsertInst(retValue, eltValues, i, "set_value");
        }
        return retValue;
    }
    
    // Otherwise we should just have a basic scalar or pointer type and we
    // can go and do the actual gather
    AddInstrumentationPoint("gather");

    llvm::Function *gather = NULL;
    // Figure out which gather function to call based on the size of
    // the elements.
    const PointerType *pt = dynamic_cast<const PointerType *>(type);
    if (pt != NULL) {
        if (g->target.is32bit)
            gather = m->module->getFunction("__pseudo_gather_32");
        else
            gather = m->module->getFunction("__pseudo_gather_64");
    }
    else if (retType == LLVMTypes::DoubleVectorType || 
             retType == LLVMTypes::Int64VectorType)
        gather = m->module->getFunction("__pseudo_gather_64");
    else if (retType == LLVMTypes::FloatVectorType || 
             retType == LLVMTypes::Int32VectorType)
        gather = m->module->getFunction("__pseudo_gather_32");
    else if (retType == LLVMTypes::Int16VectorType)
        gather = m->module->getFunction("__pseudo_gather_16");
    else {
        assert(retType == LLVMTypes::Int8VectorType);
        gather = m->module->getFunction("__pseudo_gather_8");
    }
    assert(gather != NULL);

    lvalue = addVaryingOffsetsIfNeeded(lvalue, type);

    llvm::Value *voidlvalue = BitCastInst(lvalue, LLVMTypes::VoidPointerType);
    llvm::Value *call = CallInst(gather, type, voidlvalue, mask, name);

    // Add metadata about the source file location so that the
    // optimization passes can print useful performance warnings if we
    // can't optimize out this gather
    addGSMetadata(call, currentPos);

    if (pt != NULL) {
        LLVM_TYPE_CONST llvm::Type *ptrType = 
            pt->GetAsUniformType()->LLVMType(g->ctx);
        return IntToPtrInst(VectorToArrayInst(call), ptrType,
                            "gather_bitcast");
    }
    else
        return BitCastInst(call, retType, "gather_bitcast");
}


/** Add metadata to the given instruction to encode the current source file
    position.  This data is used in the lGetSourcePosFromMetadata()
    function in opt.cpp. 
*/
void
FunctionEmitContext::addGSMetadata(llvm::Value *v, SourcePos pos) {
    llvm::Instruction *inst = llvm::dyn_cast<llvm::Instruction>(v);
    if (inst == NULL)
        return;

    llvm::Value *str = llvm::MDString::get(*g->ctx, pos.name);
    llvm::MDNode *md = llvm::MDNode::get(*g->ctx, str);
    inst->setMetadata("filename", md);

    llvm::Value *first_line = LLVMInt32(pos.first_line);
    md = llvm::MDNode::get(*g->ctx, first_line);
    inst->setMetadata("first_line", md);

    llvm::Value *first_column = LLVMInt32(pos.first_column);
    md = llvm::MDNode::get(*g->ctx, first_column);
    inst->setMetadata("first_column", md);

    llvm::Value *last_line = LLVMInt32(pos.last_line);
    md = llvm::MDNode::get(*g->ctx, last_line);
    inst->setMetadata("last_line", md);

    llvm::Value *last_column = LLVMInt32(pos.last_column);
    md = llvm::MDNode::get(*g->ctx, last_column);
    inst->setMetadata("last_column", md);
}


llvm::Value *
FunctionEmitContext::AllocaInst(LLVM_TYPE_CONST llvm::Type *llvmType, const char *name,
                                int align, bool atEntryBlock) {
    llvm::AllocaInst *inst = NULL;
    if (atEntryBlock) {
        // We usually insert it right before the jump instruction at the
        // end of allocaBlock
        llvm::Instruction *retInst = allocaBlock->getTerminator();
        assert(retInst);
        inst = new llvm::AllocaInst(llvmType, name ? name : "", retInst);
    }
    else
        // Unless the caller overrode the default and wants it in the
        // current basic block
        inst = new llvm::AllocaInst(llvmType, name ? name : "", bblock);

    // If no alignment was specified but we have an array of a uniform
    // type, then align it to 4 * the native vector width; it's not
    // unlikely that this array will be loaded into varying variables with
    // what will be aligned accesses if the uniform -> varying load is done
    // in regular chunks.
    LLVM_TYPE_CONST llvm::ArrayType *arrayType = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(llvmType);
    if (align == 0 && arrayType != NULL && 
        !llvm::isa<LLVM_TYPE_CONST llvm::VectorType>(arrayType->getElementType()))
        align = 4 * g->target.nativeVectorWidth;

    if (align != 0)
        inst->setAlignment(align);
    // Don't add debugging info to alloca instructions
    return inst;
}


/** Code to store the given varying value to the given location, only
    storing the elements that correspond to active program instances as
    given by the provided storeMask value.  Note that the lvalue is only a
    single pointer, not a varying lvalue of one pointer per program
    instance (that case is handled by scatters).
 */
void
FunctionEmitContext::maskedStore(llvm::Value *rvalue, llvm::Value *lvalue,
                                 const Type *rvalueType, 
                                 llvm::Value *storeMask) {
    if (rvalue == NULL || lvalue == NULL) {
        assert(m->errorCount > 0);
        return;
    }

    assert(llvm::isa<LLVM_TYPE_CONST llvm::PointerType>(lvalue->getType()));
    
    const CollectionType *collectionType = 
        dynamic_cast<const CollectionType *>(rvalueType);
    if (collectionType != NULL) {
        // Assigning a structure / array / vector. Handle each element
        // individually with what turns into a recursive call to
        // makedStore()
        for (int i = 0; i < collectionType->GetElementCount(); ++i) {
            llvm::Value *eltValue = ExtractInst(rvalue, i, "rvalue_member");
            llvm::Value *eltLValue = GetElementPtrInst(lvalue, 0, i, 
                                                       "struct_lvalue_ptr");
            StoreInst(eltValue, eltLValue, storeMask, 
                      collectionType->GetElementType(i));
        }
        return;
    }

    const PointerType *pt = dynamic_cast<const PointerType *>(rvalueType);
    if (pt != NULL) {
        if (g->target.is32bit) {
            rvalue = PtrToIntInst(rvalue, LLVMTypes::Int32Type, "ptr2int");
            rvalueType = AtomicType::VaryingInt32;
        }
        else {
            rvalue = PtrToIntInst(rvalue, LLVMTypes::Int64Type, "ptr2int");
            rvalueType = AtomicType::VaryingInt64;
        }
        rvalue = ArrayToVectorInst(rvalue);
    }

    // We must have a regular atomic or enumerator type at this point
    assert(dynamic_cast<const AtomicType *>(rvalueType) != NULL ||
           dynamic_cast<const EnumType *>(rvalueType) != NULL);
    rvalueType = rvalueType->GetAsNonConstType();

    llvm::Function *maskedStoreFunc = NULL;
    // Figure out if we need a 8, 16, 32 or 64-bit masked store.
    if (rvalueType == AtomicType::VaryingDouble || 
        rvalueType == AtomicType::VaryingInt64 ||
        rvalueType == AtomicType::VaryingUInt64) {
        maskedStoreFunc = m->module->getFunction("__pseudo_masked_store_64");
        lvalue = BitCastInst(lvalue, LLVMTypes::Int64VectorPointerType, 
                             "lvalue_to_int64vecptr");
        rvalue = BitCastInst(rvalue, LLVMTypes::Int64VectorType, 
                             "rvalue_to_int64");
    }
    else if (rvalueType == AtomicType::VaryingFloat ||
             rvalueType == AtomicType::VaryingBool ||
             rvalueType == AtomicType::VaryingInt32 ||
             rvalueType == AtomicType::VaryingUInt32 ||
             dynamic_cast<const EnumType *>(rvalueType) != NULL) {
        maskedStoreFunc = m->module->getFunction("__pseudo_masked_store_32");
        lvalue = BitCastInst(lvalue, LLVMTypes::Int32VectorPointerType, 
                             "lvalue_to_int32vecptr");
        if (rvalueType == AtomicType::VaryingFloat)
            rvalue = BitCastInst(rvalue, LLVMTypes::Int32VectorType, 
                                 "rvalue_to_int32");
    }
    else if (rvalueType == AtomicType::VaryingInt16 ||
             rvalueType == AtomicType::VaryingUInt16) {
        maskedStoreFunc = m->module->getFunction("__pseudo_masked_store_16");
        lvalue = BitCastInst(lvalue, LLVMTypes::Int16VectorPointerType, 
                             "lvalue_to_int16vecptr");
    }
    else if (rvalueType == AtomicType::VaryingInt8 ||
             rvalueType == AtomicType::VaryingUInt8) {
        maskedStoreFunc = m->module->getFunction("__pseudo_masked_store_8");
        lvalue = BitCastInst(lvalue, LLVMTypes::Int8VectorPointerType, 
                             "lvalue_to_int8vecptr");
    }

    std::vector<llvm::Value *> args;
    args.push_back(lvalue);
    args.push_back(rvalue);
    args.push_back(storeMask);
    CallInst(maskedStoreFunc, AtomicType::Void, args);
}



/** Scatter the given varying value to the locations given by the varying
    lvalue (which should be an array of pointers with size equal to the
    target's vector width.  We want to store each rvalue element at the
    corresponding pointer's location, *if* the mask for the corresponding
    program instance are on.  If they're off, don't do anything.  
*/
void
FunctionEmitContext::scatter(llvm::Value *rvalue, llvm::Value *lvalue, 
                             llvm::Value *storeMask, const Type *rvalueType) {
    assert(rvalueType->IsVaryingType());
    assert(llvm::isa<LLVM_TYPE_CONST llvm::ArrayType>(lvalue->getType()));

    const StructType *structType = dynamic_cast<const StructType *>(rvalueType);
    if (structType) {
        // Scatter the struct elements individually
        for (int i = 0; i < structType->GetElementCount(); ++i) {
            llvm::Value *lv = GetElementPtrInst(lvalue, 0, i);
            llvm::Value *rv = ExtractInst(rvalue, i);
            scatter(rv, lv, storeMask, structType->GetElementType(i));
        }
        return;
    }

    const VectorType *vt = dynamic_cast<const VectorType *>(rvalueType);
    if (vt) {
        // FIXME: yuck.  Change lvalues to be pointers to arrays so that
        // the GEP stuff in the loop below ends up computing pointers based
        // on elements in the vectors rather than incorrectly advancing to
        // the next vector...
        LLVM_TYPE_CONST llvm::Type *eltType = 
            vt->GetBaseType()->GetAsUniformType()->LLVMType(g->ctx);
        lvalue = BitCastInst(lvalue, llvm::PointerType::get(llvm::ArrayType::get(eltType, 0), 0));

        for (int i = 0; i < vt->GetElementCount(); ++i) {
            llvm::Value *lv = GetElementPtrInst(lvalue, 0, i);
            llvm::Value *rv = ExtractInst(rvalue, i);
            scatter(rv, lv, storeMask, vt->GetElementType());
        }
        return;
    }

    // I think this should be impossible
    assert(dynamic_cast<const ArrayType *>(rvalueType) == NULL);

    const PointerType *pt = dynamic_cast<const PointerType *>(rvalueType);

    // And everything should be a pointer or atomic from here on out...
    assert(pt != NULL || 
           dynamic_cast<const AtomicType *>(rvalueType) != NULL);

    llvm::Function *func = NULL;
    LLVM_TYPE_CONST llvm::Type *type = rvalue->getType();
    if (pt != NULL) {
        if (g->target.is32bit) {
            rvalue = PtrToIntInst(rvalue, LLVMTypes::Int32Type);
            rvalue = ArrayToVectorInst(rvalue);
            func = m->module->getFunction("__pseudo_scatter_32");
        }
        else {
            rvalue = PtrToIntInst(rvalue, LLVMTypes::Int64Type);
            rvalue = ArrayToVectorInst(rvalue);
            func = m->module->getFunction("__pseudo_scatter_64");
        }
    }
    else if (type == LLVMTypes::DoubleVectorType || 
        type == LLVMTypes::Int64VectorType) {
        func = m->module->getFunction("__pseudo_scatter_64");
        rvalue = BitCastInst(rvalue, LLVMTypes::Int64VectorType, "rvalue2int");
    }
    else if (type == LLVMTypes::FloatVectorType || 
             type == LLVMTypes::Int32VectorType) {
        func = m->module->getFunction("__pseudo_scatter_32");
        rvalue = BitCastInst(rvalue, LLVMTypes::Int32VectorType, "rvalue2int");
    }
    else if (type == LLVMTypes::Int16VectorType)
        func = m->module->getFunction("__pseudo_scatter_16");
    else if (type == LLVMTypes::Int8VectorType)
        func = m->module->getFunction("__pseudo_scatter_8");
    assert(func != NULL);
    
    AddInstrumentationPoint("scatter");

    lvalue = addVaryingOffsetsIfNeeded(lvalue, rvalueType);

    llvm::Value *voidlvalue = BitCastInst(lvalue, LLVMTypes::VoidPointerType);
    std::vector<llvm::Value *> args;
    args.push_back(voidlvalue);
    args.push_back(rvalue);
    args.push_back(storeMask);
    llvm::Value *inst = CallInst(func, AtomicType::Void, args);
    addGSMetadata(inst, currentPos);
}


void
FunctionEmitContext::StoreInst(llvm::Value *rvalue, llvm::Value *lvalue,
                               const char *name) {
    if (rvalue == NULL || lvalue == NULL) {
        // may happen due to error elsewhere
        assert(m->errorCount > 0);
        return;
    }

    llvm::Instruction *inst;
    if (llvm::isa<llvm::VectorType>(rvalue->getType()))
        // Specify an unaligned store, since we don't know that the lvalue
        // will in fact be aligned to a vector width here.  (Actually
        // should be aligned to the alignment of the vector elment type...)
        inst = new llvm::StoreInst(rvalue, lvalue, false /* not volatile */,
                                   1, bblock);
    else
        inst = new llvm::StoreInst(rvalue, lvalue, bblock);

    AddDebugPos(inst);
}


void
FunctionEmitContext::StoreInst(llvm::Value *rvalue, llvm::Value *lvalue,
                               llvm::Value *storeMask, const Type *rvalueType,
                               const char *name) {
    if (rvalue == NULL || lvalue == NULL) {
        // may happen due to error elsewhere
        assert(m->errorCount > 0);
        return;
    }

    // Figure out what kind of store we're doing here
    if (rvalueType->IsUniformType()) {
        // The easy case; a regular store, natural alignment is fine
        llvm::Instruction *si = new llvm::StoreInst(rvalue, lvalue, bblock);
        AddDebugPos(si);
    }
    else if (llvm::isa<LLVM_TYPE_CONST llvm::ArrayType>(lvalue->getType()))
        // We have a varying lvalue (an array of pointers), so it's time to
        // scatter
        scatter(rvalue, lvalue, storeMask, rvalueType);
    else if (storeMask == LLVMMaskAllOn) {
        // Otherwise it is a masked store unless we can determine that the
        // mask is all on...
        StoreInst(rvalue, lvalue, name);
    }
    else
        maskedStore(rvalue, lvalue, rvalueType, storeMask);
}


void
FunctionEmitContext::BranchInst(llvm::BasicBlock *dest) {
    llvm::Instruction *b = llvm::BranchInst::Create(dest, bblock);
    AddDebugPos(b);
}


void
FunctionEmitContext::BranchInst(llvm::BasicBlock *trueBlock, 
                                llvm::BasicBlock *falseBlock,
                                llvm::Value *test) {
    if (test == NULL) {
        assert(m->errorCount > 0);
        return;
    }

    llvm::Instruction *b = 
        llvm::BranchInst::Create(trueBlock, falseBlock, test, bblock);
    AddDebugPos(b);
}


llvm::Value *
FunctionEmitContext::ExtractInst(llvm::Value *v, int elt, const char *name) {
    if (v == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    llvm::Instruction *ei = NULL;
    if (llvm::isa<LLVM_TYPE_CONST llvm::VectorType>(v->getType()))
        ei = llvm::ExtractElementInst::Create(v, LLVMInt32(elt), 
                                              name ? name : "extract", bblock);
    else
        ei = llvm::ExtractValueInst::Create(v, elt, name ? name : "extract",
                                            bblock);
    AddDebugPos(ei);
    return ei;
}


llvm::Value *
FunctionEmitContext::InsertInst(llvm::Value *v, llvm::Value *eltVal, int elt, 
                                const char *name) {
    if (v == NULL || eltVal == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    llvm::Instruction *ii = NULL;
    if (llvm::isa<LLVM_TYPE_CONST llvm::VectorType>(v->getType()))
        ii = llvm::InsertElementInst::Create(v, eltVal, LLVMInt32(elt), 
                                             name ? name : "insert", bblock);
    else
        ii = llvm::InsertValueInst::Create(v, eltVal, elt, 
                                           name ? name : "insert", bblock);
    AddDebugPos(ii);
    return ii;
}


llvm::PHINode *
FunctionEmitContext::PhiNode(LLVM_TYPE_CONST llvm::Type *type, int count, 
                             const char *name) {
    llvm::PHINode *pn = llvm::PHINode::Create(type, 
#if defined(LLVM_3_0) || defined(LLVM_3_0svn) || defined(LLVM_3_1svn)
                                              count, 
#endif // LLVM_3_0
                                              name ? name : "phi", bblock);
    AddDebugPos(pn);
    return pn;
}


llvm::Instruction *
FunctionEmitContext::SelectInst(llvm::Value *test, llvm::Value *val0,
                                llvm::Value *val1, const char *name) {
    if (test == NULL || val0 == NULL || val1 == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    llvm::Instruction *inst = 
        llvm::SelectInst::Create(test, val0, val1, name ? name : "select", 
                                 bblock);
    AddDebugPos(inst);
    return inst;
}


/* Given a value representing a function to be called or possibly-varying
   pointer to a function to be called, figure out how many arguments the
   function has. */
static unsigned int
lCalleeArgCount(llvm::Value *callee) {
    LLVM_TYPE_CONST llvm::FunctionType *ft = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::FunctionType>(callee->getType());
    if (ft == NULL) {
        LLVM_TYPE_CONST llvm::PointerType *pt =
            llvm::dyn_cast<LLVM_TYPE_CONST llvm::PointerType>(callee->getType());
        if (pt == NULL) {
            // varying...
            LLVM_TYPE_CONST llvm::ArrayType *at =
                llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(callee->getType());
            assert(at != NULL);
            pt = llvm::dyn_cast<LLVM_TYPE_CONST llvm::PointerType>(at->getElementType());
            assert(pt != NULL);
        }
        ft = llvm::dyn_cast<LLVM_TYPE_CONST llvm::FunctionType>(pt->getElementType());
    }
    assert(ft != NULL);
    return ft->getNumParams();
}


llvm::Value *
FunctionEmitContext::CallInst(llvm::Value *func, const Type *returnType,
                              const std::vector<llvm::Value *> &args,
                              const char *name) {
    if (func == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    std::vector<llvm::Value *> argVals = args;
    // Most of the time, the mask is passed as the last argument.  this
    // isn't the case for things like intrinsics, builtins, and extern "C"
    // functions from the application.  Add the mask if it's needed.
    unsigned int calleeArgCount = lCalleeArgCount(func);
    assert(argVals.size() + 1 == calleeArgCount ||
           argVals.size() == calleeArgCount);
    if (argVals.size() + 1 == calleeArgCount)
        argVals.push_back(GetFullMask());

    LLVM_TYPE_CONST llvm::Type *funcType = func->getType();
    LLVM_TYPE_CONST llvm::ArrayType *funcArrayType = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(funcType);

    if (funcArrayType == NULL) {
        // Regular 'uniform' function call--just one function or function
        // pointer, so just emit the IR directly.
#if defined(LLVM_3_0) || defined(LLVM_3_0svn) || defined(LLVM_3_1svn)
        llvm::Instruction *ci = 
            llvm::CallInst::Create(func, argVals, name ? name : "", bblock);
#else
        llvm::Instruction *ci = 
            llvm::CallInst::Create(func, argVals.begin(), argVals.end(), 
                                   name ? name : "", bblock);
#endif
        AddDebugPos(ci);
        return ci;
    }
    else {
        // Emit the code for a varying function call, where we have an
        // array of function pointers, one for each program instance.  The
        // basic strategy is that we go through the function pointers, and
        // for the executing program instances, for each unique function
        // pointer that's in the array, call that function with a mask
        // equal to the set of active program instances that also have that
        // function pointer.  When all unique function pointers have been
        // called, we're done.

        llvm::BasicBlock *bbTest = CreateBasicBlock("varying_funcall_test");
        llvm::BasicBlock *bbCall = CreateBasicBlock("varying_funcall_call");
        llvm::BasicBlock *bbDone = CreateBasicBlock("varying_funcall_done");

        llvm::Value *origMask = GetInternalMask();

        // First allocate memory to accumulate the various lanes' return
        // values...
        LLVM_TYPE_CONST llvm::Type *llvmReturnType = returnType->LLVMType(g->ctx);
        llvm::Value *resultPtr = NULL;
        if (llvmReturnType->isVoidTy() == false)
            resultPtr = AllocaInst(llvmReturnType);

        // Store the function pointers into an array so that we can index
        // into them..
        llvm::Value *funcPtrArray = AllocaInst(funcType);
        StoreInst(func, funcPtrArray);

        // The memory pointed to by maskPointer tracks the set of program
        // instances for which we still need to call the function they are
        // pointing to.  It starts out initialized with the mask of
        // currently running program instances.
        llvm::Value *maskPtr = AllocaInst(LLVMTypes::MaskType);
        StoreInst(GetFullMask(), maskPtr);

        // And now we branch to the test to see if there's more work to be
        // done.
        BranchInst(bbTest);

        // bbTest: are any lanes of the mask still on?  If so, jump to
        // bbCall
        SetCurrentBasicBlock(bbTest); {
            llvm::Value *maskLoad = LoadInst(maskPtr, NULL, NULL);
            llvm::Value *any = Any(maskLoad);
            BranchInst(bbCall, bbDone, any);
        }

        // bbCall: this is the body of the loop that calls out to one of
        // the active function pointer values.
        SetCurrentBasicBlock(bbCall); {
            // Figure out the first lane that still needs its function
            // pointer to be called.
            llvm::Value *currentMask = LoadInst(maskPtr, NULL, NULL);
            llvm::Function *cttz = m->module->getFunction("__count_trailing_zeros");
            assert(cttz != NULL);
            llvm::Value *firstLane = CallInst(cttz, AtomicType::UniformInt32, 
                                              LaneMask(currentMask), "first_lane");

            // Get the pointer to the function we're going to call this time through:
            // ftpr = funcPtrArray[firstLane]
            llvm::Value *fpOffset = 
                GetElementPtrInst(funcPtrArray, LLVMInt32(0), firstLane,
                                  "func_offset_ptr");
            llvm::Value *fptr = LoadInst(fpOffset, NULL, NULL);

            // Smear it out into an array of function pointers
            llvm::Value *fptrSmear = SmearScalar(fptr, "func_ptr");

            // Now convert the smeared array of function pointers and the
            // given array of function pointers to vectors of int32s or
            // int64s, where the pointer has been cast to an int of the
            // appropraite size for the compilation target.
            LLVM_TYPE_CONST llvm::Type *ptrIntType = g->target.is32bit ?
                LLVMTypes::Int32Type : LLVMTypes::Int64Type;
            llvm::Value *fpSmearAsVec = 
                ArrayToVectorInst(PtrToIntInst(fptrSmear, ptrIntType));
            llvm::Value *fpOrigAsVec = 
                ArrayToVectorInst(PtrToIntInst(func, ptrIntType));

            // fpOverlap = (fpSmearAsVec == fpOrigAsVec).  This gives us a
            // mask for the set of program instances that have the same
            // value for their function pointer.
            llvm::Value *fpOverlap = 
                CmpInst(llvm::Instruction::ICmp, llvm::CmpInst::ICMP_EQ,
                        fpSmearAsVec, fpOrigAsVec);
            fpOverlap = I1VecToBoolVec(fpOverlap);

            // Figure out the mask to use when calling the function
            // pointer: we need to AND the current execution mask to handle
            // the case of any non-running program instances that happen to
            // have this function pointer value.
            // callMask = (currentMask & fpOverlap)
            llvm::Value *callMask = 
                BinaryOperator(llvm::Instruction::And, currentMask, fpOverlap,
                               "call_mask");

            // Set the mask
            SetInternalMask(callMask);

            // Call the function: callResult = call ftpr(args, args, call mask)
            llvm::Value *callResult = CallInst(fptr, returnType, args, name);

            // Now, do a masked store into the memory allocated to
            // accumulate the result using the call mask.
            if (callResult != NULL) {
                assert(resultPtr != NULL);
                StoreInst(callResult, resultPtr, callMask, returnType);
            }
            else
                assert(resultPtr == NULL);

            // Update the mask to turn off the program instances for which
            // we just called the function.
            // currentMask = currentMask & ~callmask
            llvm::Value *notCallMask = 
                BinaryOperator(llvm::Instruction::Xor, callMask, LLVMMaskAllOn,
                               "~callMask");
            currentMask = BinaryOperator(llvm::Instruction::And, currentMask, 
                                         notCallMask, "currentMask&~callMask");
            StoreInst(currentMask, maskPtr);

            // And go back to the test to see if we need to do another
            // call.
            BranchInst(bbTest);
        }

        // bbDone: We're all done; clean up and return the result we've
        // accumulated in the result memory.
        SetCurrentBasicBlock(bbDone);
        SetInternalMask(origMask);
        return LoadInst(resultPtr, NULL, NULL);
    }
}


llvm::Value *
FunctionEmitContext::CallInst(llvm::Value *func, const Type *returnType,
                              llvm::Value *arg, const char *name) {
    std::vector<llvm::Value *> args;
    args.push_back(arg);
    return CallInst(func, returnType, args, name);
}


llvm::Value *
FunctionEmitContext::CallInst(llvm::Value *func, const Type *returnType,
                              llvm::Value *arg0, llvm::Value *arg1, 
                              const char *name) {
    std::vector<llvm::Value *> args;
    args.push_back(arg0);
    args.push_back(arg1);
    return CallInst(func, returnType, args, name);
}


llvm::Instruction *
FunctionEmitContext::ReturnInst() {
    if (launchedTasks)
        // Add a sync call at the end of any function that launched tasks
        SyncInst();

    llvm::Instruction *rinst = NULL;
    if (returnValuePtr != NULL) {
        // We have value(s) to return; load them from their storage
        // location
        llvm::Value *retVal = LoadInst(returnValuePtr, NULL, NULL,
                                       "return_value");
        rinst = llvm::ReturnInst::Create(*g->ctx, retVal, bblock);
    }
    else {
        assert(function->GetReturnType() == AtomicType::Void);
        rinst = llvm::ReturnInst::Create(*g->ctx, bblock);
    }

    AddDebugPos(rinst);
    bblock = NULL;
    return rinst;
}


llvm::Value *
FunctionEmitContext::LaunchInst(llvm::Value *callee, 
                                std::vector<llvm::Value *> &argVals,
                                llvm::Value *launchCount) {
    if (callee == NULL) {
        assert(m->errorCount > 0);
        return NULL;
    }

    launchedTasks = true;

    assert(llvm::isa<llvm::Function>(callee));
    LLVM_TYPE_CONST llvm::Type *argType = 
        (llvm::dyn_cast<llvm::Function>(callee))->arg_begin()->getType();
    assert(llvm::PointerType::classof(argType));
    LLVM_TYPE_CONST llvm::PointerType *pt = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::PointerType>(argType);
    assert(llvm::StructType::classof(pt->getElementType()));
    LLVM_TYPE_CONST llvm::StructType *argStructType = 
        static_cast<LLVM_TYPE_CONST llvm::StructType *>(pt->getElementType());
    assert(argStructType->getNumElements() == argVals.size() + 1);

    llvm::Function *falloc = m->module->getFunction("ISPCAlloc");
    assert(falloc != NULL);
    int align = 4 * RoundUpPow2(g->target.nativeVectorWidth);
    std::vector<llvm::Value *> allocArgs;
    allocArgs.push_back(launchGroupHandlePtr);
    allocArgs.push_back(g->target.SizeOf(argStructType));
    allocArgs.push_back(LLVMInt32(align));
    llvm::Value *voidmem = CallInst(falloc, NULL, allocArgs, "args_ptr");
    llvm::Value *argmem = BitCastInst(voidmem, pt);

    // Copy the values of the parameters into the appropriate place in
    // the argument block
    for (unsigned int i = 0; i < argVals.size(); ++i) {
        llvm::Value *ptr = GetElementPtrInst(argmem, 0, i, "funarg");
        // don't need to do masked store here, I think
        StoreInst(argVals[i], ptr);
    }

    // copy in the mask
    llvm::Value *mask = GetFullMask();
    llvm::Value *ptr = GetElementPtrInst(argmem, 0, argVals.size(),
                                         "funarg_mask");
    StoreInst(mask, ptr);

    // And emit the call to the user-supplied task launch function, passing
    // a pointer to the task function being called and a pointer to the
    // argument block we just filled in
    llvm::Value *fptr = BitCastInst(callee, LLVMTypes::VoidPointerType);
    llvm::Function *flaunch = m->module->getFunction("ISPCLaunch");
    assert(flaunch != NULL);
    std::vector<llvm::Value *> args;
    args.push_back(launchGroupHandlePtr);
    args.push_back(fptr);
    args.push_back(voidmem);
    args.push_back(launchCount);
    return CallInst(flaunch, AtomicType::Void, args, "");
}


void
FunctionEmitContext::SyncInst() {
    llvm::Value *launchGroupHandle = LoadInst(launchGroupHandlePtr, NULL, NULL);
    llvm::Value *nullPtrValue = llvm::Constant::getNullValue(LLVMTypes::VoidPointerType);
    llvm::Value *nonNull = CmpInst(llvm::Instruction::ICmp,
                                   llvm::CmpInst::ICMP_NE,
                                   launchGroupHandle, nullPtrValue);
    llvm::BasicBlock *bSync = CreateBasicBlock("call_sync");
    llvm::BasicBlock *bPostSync = CreateBasicBlock("post_sync");
    BranchInst(bSync, bPostSync, nonNull);

    SetCurrentBasicBlock(bSync);
    llvm::Function *fsync = m->module->getFunction("ISPCSync");
    if (fsync == NULL)
        FATAL("Couldn't find ISPCSync declaration?!");
    CallInst(fsync, AtomicType::Void, launchGroupHandle, "");
    BranchInst(bPostSync);

    SetCurrentBasicBlock(bPostSync);
}


/** When we gathering from or scattering to a varying atomic type, we need
    to add an appropraite toffset to the final address for each lane right
    before we use it.  Given a varying pointer we're about to use and its
    type, this function determines whether these offsets are needed and
    returns an updated pointer that incorporates these offsets if needed.
 */
llvm::Value *
FunctionEmitContext::addVaryingOffsetsIfNeeded(llvm::Value *ptr, const Type *type) {
    // We should only have varying pointers here, which are represented as
    // arrays of pointers in ispc.
    LLVM_TYPE_CONST llvm::ArrayType *at = 
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::ArrayType>(ptr->getType());
    assert(at != NULL);
    LLVM_TYPE_CONST llvm::PointerType *pt =
        llvm::dyn_cast<LLVM_TYPE_CONST llvm::PointerType>(at->getElementType());
    assert(pt != NULL);

    // If we have pointers to vector types, e.g. [8 x <8 x float> *], then
    // the data we're gathering from/scattering to is varying in memory.
    // If we have pointers to scalar types, e.g. [8 x float *], then the
    // data is uniform in memory and doesn't need any additional offsets.
    if (pt->getElementType()->isIntegerTy() || 
        pt->getElementType()->isFloatingPointTy() ||
        pt->getElementType()->isPointerTy())
        return ptr;

    llvm::Value *varyingOffsets = llvm::UndefValue::get(LLVMTypes::Int32VectorType);
    for (int i = 0; i < g->target.vectorWidth; ++i)
        varyingOffsets = InsertInst(varyingOffsets, LLVMInt32(i), i,
                                    "varying_delta");

    // Cast the pointer type to the corresponding uniform type--e.g. cast
    // <8 x float> * to float *s.
    LLVM_TYPE_CONST llvm::Type *unifType = type->GetAsUniformType()->LLVMType(g->ctx);
    LLVM_TYPE_CONST llvm::PointerType *ptrCastType = 
        llvm::PointerType::get(llvm::ArrayType::get(unifType, 0), 0);
    ptr = BitCastInst(ptr, ptrCastType, "ptr2unif");

    // And now we can do the per-lane offsets...
    return GetElementPtrInst(ptr, LLVMInt32(0), varyingOffsets);
}
