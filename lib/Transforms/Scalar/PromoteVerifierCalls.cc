#include "seahorn/Transforms/Scalar/PromoteVerifierCalls.hh"
#include "seahorn/Analysis/SeaBuiltinsInfo.hh"

#include "llvm/IR/CallSite.h"
#include "llvm/IR/InstIterator.h"

#include "llvm/IR/IRBuilder.h"

#include "boost/range.hpp"
#include "llvm/Support/raw_ostream.h"

#include "seahorn/Support/SeaDebug.h"
#include "seahorn/Support/SeaLog.hh"

#include "llvm/Analysis/CallGraph.h"
using namespace llvm;

namespace {

/// Converts \p arg to Boolean (i.e., i1) type
///
/// If necessary, inserts additional instructions before the given instruction
Value *coerceToBool(Value *arg, IRBuilder<> &builder, Instruction &I) {
  assert(arg);
  // strip zext if there is one
  if (const ZExtInst *ze = dyn_cast<const ZExtInst>(arg))
    arg = ze->getOperand(0);

  builder.SetInsertPoint(&I);
  // -- convert to Boolean if needed
  if (!arg->getType()->isIntegerTy(1))
    arg = builder.CreateICmpNE(arg, ConstantInt::get(arg->getType(), 0));
  return arg;
}
} // namespace
namespace seahorn {

char PromoteVerifierCalls::ID = 0;

bool PromoteVerifierCalls::runOnModule(Module &M) {
  LOG("pvc", errs() << "Running promote-verifier-calls pass\n";);

  auto &SBI = getAnalysis<SeaBuiltinsInfoWrapperPass>().getSBI();
  using SBIOp = SeaBuiltinsOp;
  m_assumeFn = SBI.mkSeaBuiltinFn(SBIOp::ASSUME, M);
  Function *assumeNotFn = SBI.mkSeaBuiltinFn(SBIOp::ASSUME_NOT, M);
  m_assertFn = SBI.mkSeaBuiltinFn(SBIOp::ASSERT, M);
  m_assertNotFn = SBI.mkSeaBuiltinFn(SBIOp::ASSERT_NOT, M);
  m_failureFn = SBI.mkSeaBuiltinFn(SBIOp::FAIL, M);
  m_errorFn = SBI.mkSeaBuiltinFn(SBIOp::ERROR, M);
  m_is_deref = SBI.mkSeaBuiltinFn(SBIOp::IS_DEREFERENCEABLE, M);
  m_assert_if = SBI.mkSeaBuiltinFn(SBIOp::ASSERT_IF, M);
  m_upredAssumeFn = SBI.mkSeaBuiltinFn(SBIOp::UPRED_ASSUME, M);
  m_upredAssertFn = SBI.mkSeaBuiltinFn(SBIOp::UPRED_ASSERT, M);
  m_is_modified = SBI.mkSeaBuiltinFn(SBIOp::IS_MODIFIED, M);
  m_reset_modified = SBI.mkSeaBuiltinFn(SBIOp::RESET_MODIFIED, M);
  m_is_read = SBI.mkSeaBuiltinFn(SBIOp::IS_READ, M);
  m_reset_read = SBI.mkSeaBuiltinFn(SBIOp::RESET_READ, M);
  m_is_alloc = SBI.mkSeaBuiltinFn(SBIOp::IS_ALLOC, M);
  m_tracking_on = SBI.mkSeaBuiltinFn(SBIOp::TRACKING_ON, M);
  m_tracking_off = SBI.mkSeaBuiltinFn(SBIOp::TRACKING_OFF, M);
  m_free = SBI.mkSeaBuiltinFn(SBIOp::FREE, M);

  // XXX DEPRECATED
  // Do not keep unused functions in llvm.used

  /* add our functions to llvm used */
  GlobalVariable *LLVMUsed = M.getGlobalVariable("llvm.used");
  std::vector<Constant *> MergedVars;
  if (LLVMUsed) {
    // Collect the existing members of llvm.used except sea
    // functions
    ConstantArray *Inits = cast<ConstantArray>(LLVMUsed->getInitializer());
    for (unsigned I = 0, E = Inits->getNumOperands(); I != E; ++I) {
      Value *V = Inits->getOperand(I)->stripPointerCasts();
      MergedVars.push_back(Inits->getOperand(I));
    }
    LLVMUsed->eraseFromParent();
  }
  // re-create llvm.used
  Type *i8PTy = Type::getInt8PtrTy(M.getContext());
  MergedVars.push_back(
      ConstantExpr::getBitCast(cast<llvm::Constant>(m_assumeFn), i8PTy));
  MergedVars.push_back(
      ConstantExpr::getBitCast(cast<llvm::Constant>(assumeNotFn), i8PTy));
  MergedVars.push_back(
      ConstantExpr::getBitCast(cast<llvm::Constant>(m_errorFn), i8PTy));
  MergedVars.push_back(
      ConstantExpr::getBitCast(cast<llvm::Constant>(m_failureFn), i8PTy));
  ArrayType *ATy = ArrayType::get(i8PTy, MergedVars.size());
  LLVMUsed = new llvm::GlobalVariable(
      M, ATy, false, llvm::GlobalValue::AppendingLinkage,
      llvm::ConstantArray::get(ATy, MergedVars), "llvm.used");
  LLVMUsed->setSection("llvm.metadata");

  // XXX Not sure how useful this is, consider removing
  CallGraphWrapperPass *cgwp = getAnalysisIfAvailable<CallGraphWrapperPass>();
  if (CallGraph *cg = cgwp ? &cgwp->getCallGraph() : nullptr) {
    cg->getOrInsertFunction(m_assumeFn);
    cg->getOrInsertFunction(assumeNotFn);
    cg->getOrInsertFunction(m_assertFn);
    cg->getOrInsertFunction(m_assertNotFn);
    cg->getOrInsertFunction(m_errorFn);
    cg->getOrInsertFunction(m_failureFn);
  }

  for (auto &F : M)
    runOnFunction(F);
  return true;
}

bool PromoteVerifierCalls::runOnFunction(Function &F) {
  CallGraphWrapperPass *cgwp = getAnalysisIfAvailable<CallGraphWrapperPass>();
  CallGraph *cg = cgwp ? &cgwp->getCallGraph() : nullptr;

  SmallVector<Instruction *, 16> toKill;

  std::map<StringRef, std::pair<Function *, unsigned>> functionMap = {
      {"sea_is_dereferenceable", {m_is_deref, 2}},
      {"sea_is_modified", {m_is_modified, 1}},
      {"sea_reset_modified", {m_reset_modified, 1}},
      {"sea_is_read", {m_is_read, 1}},
      {"sea_reset_read", {m_reset_read, 1}},
      {"sea_is_alloc", {m_is_alloc, 1}},
      {"sea_tracking_on", {m_tracking_on, 0}},
      {"sea_tracking_off", {m_tracking_off, 0}},
      {"sea_free", {m_free, 1}}};

  auto replaceFnWithOneArg = [](Instruction &I,
                                std::pair<Function *, unsigned> f, Function &F,
                                SmallVector<Instruction *, 16> &toKill,
                                CallGraph *cg) {
    IRBuilder<> Builder(F.getContext());
    Builder.SetInsertPoint(&I);
    CallSite CS(&I);
    CallInst *ci;
    if (f.second == 0) {
      ci = Builder.CreateCall(f.first);
    } else if (f.second == 1) {
      ci = Builder.CreateCall(f.first, {CS.getArgument(0)});
    } else if (f.second == 2) {
      ci = Builder.CreateCall(f.first, {CS.getArgument(0), CS.getArgument(1)});
    }
    assert(ci);
    if (cg)
      (*cg)[&F]->addCalledFunction(ci, (*cg)[ci->getCalledFunction()]);
    I.replaceAllUsesWith(ci);
    toKill.push_back(&I);
  };

  bool Changed = false;
  for (auto &I : boost::make_iterator_range(inst_begin(F), inst_end(F))) {
    if (!isa<CallInst>(&I))
      continue;

    // // -- look through pointer casts
    // Value *v = I.stripPointerCasts ();
    // CallSite CS (const_cast<Value*> (v));

    CallSite CS(&I);
    const Function *fn = CS.getCalledFunction();

    // -- check if this is a call through a pointer cast
    if (!fn && CS.getCalledValue())
      fn = dyn_cast<const Function>(CS.getCalledValue()->stripPointerCasts());

    if (fn && (fn->getName().equals("__VERIFIER_assume") ||
               fn->getName().equals("__VERIFIER_assert") ||
               fn->getName().equals("__VERIFIER_assert_not") ||
               // Uninterpreted predicates.
               fn->getName().equals("__VERIFIER_upred_assume") ||
               fn->getName().equals("__VERIFIER_upred_assert") ||
               // CBMC
               fn->getName().equals("__CPROVER_assume") ||
               /** pagai embedded invariants */
               fn->getName().equals("llvm.invariant") ||
               /** my suggested name for pagai invariants */
               fn->getName().equals("pagai.invariant"))) {
      Function *nfn;
      if (fn->getName().equals("__VERIFIER_assume"))
        nfn = m_assumeFn;
      else if (fn->getName().equals("llvm.invariant"))
        nfn = m_assumeFn;
      else if (fn->getName().equals("pagai.invariant"))
        nfn = m_assumeFn;
      else if (fn->getName().equals("__VERIFIER_assert"))
        nfn = m_assertFn;
      else if (fn->getName().equals("__VERIFIER_assert_not"))
        nfn = m_assertNotFn;
      else if (fn->getName().equals("__VERIFIER_upred_assume"))
        nfn = m_upredAssumeFn;
      else if (fn->getName().equals("__VERIFIER_upred_assert"))
        nfn = m_upredAssertFn;
      else if (fn->getName().equals("__CPROVER_assume"))
        nfn = m_assumeFn;
      else
        continue;

      IRBuilder<> Builder(F.getContext());
      auto *cond = coerceToBool(CS.getArgument(0), Builder, I);
      CallInst *ci = Builder.CreateCall(nfn, cond);
      if (cg)
        (*cg)[&F]->addCalledFunction(ci, (*cg)[ci->getCalledFunction()]);

      toKill.push_back(&I);
    } else if (fn && fn->getName().equals("__VERIFIER_error")) {
      IRBuilder<> Builder(F.getContext());
      Builder.SetInsertPoint(&I);
      CallInst *ci = Builder.CreateCall(m_errorFn);
      ci->setDebugLoc(I.getDebugLoc());
      if (cg)
        (*cg)[&F]->addCalledFunction(ci, (*cg)[ci->getCalledFunction()]);

      toKill.push_back(&I);
    } else if (fn && (fn->getName().equals("__SEAHORN_fail") ||
                      /* map __llbmc_assert to seahorn.fail to support legacy
                         frontend */
                      fn->getName().equals("__llbmc_assert"))) {
      Function *main = F.getParent()->getFunction("main");
      if (!main || main != &F) {
        errs() << "__SEAHORN_fail can only be called from the main function.\n";
        return false;
      }

      IRBuilder<> Builder(F.getContext());
      Builder.SetInsertPoint(&I);
      CallInst *ci = Builder.CreateCall(m_failureFn);
      ci->setDebugLoc(I.getDebugLoc());
      if (cg)
        (*cg)[&F]->addCalledFunction(ci, (*cg)[ci->getCalledFunction()]);

      toKill.push_back(&I);
    } else if (fn && functionMap.count(fn->getName())) {
      replaceFnWithOneArg(I, functionMap.at(fn->getName()), F, toKill, cg);
    } else if (fn && (fn->getName().equals(
                         "__VERIFIER_assert_if"))) { // sea_assert_if is the
                                                     // user facing name
      IRBuilder<> Builder(F.getContext());
      // arg0: antecedent
      // arg1: consequent
      auto *arg0 = coerceToBool(CS.getArgument(0), Builder, I);
      auto *arg1 = coerceToBool(CS.getArgument(1), Builder, I);

      CallInst *ci = Builder.CreateCall(m_assert_if, {arg0, arg1});
      if (cg)
        (*cg)[&F]->addCalledFunction(ci, (*cg)[ci->getCalledFunction()]);

      I.replaceAllUsesWith(ci);
      toKill.push_back(&I);
    }
  }

  for (auto *I : toKill)
    I->eraseFromParent();

  return Changed;
}

void PromoteVerifierCalls::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<SeaBuiltinsInfoWrapperPass>();
  AU.setPreservesAll();
}

} // namespace seahorn
namespace seahorn {
Pass *createPromoteVerifierCallsPass() { return new PromoteVerifierCalls(); }
} // namespace seahorn

static llvm::RegisterPass<seahorn::PromoteVerifierCalls>
    X("promote-verifier", "Promote all verifier related function");
