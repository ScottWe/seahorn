#include "seahorn/Analysis/SeaBuiltinsInfo.hh"

#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

/* This pass ensures that a block contains at most one call to
   verifier.upred.assert. This is useful for instrumenting predicate rules.
*/
namespace seahorn {

class EndBlockOnUpredAssert : public FunctionPass {
public:
  static char ID;

  EndBlockOnUpredAssert() : FunctionPass (ID) {}

  virtual bool runOnFunction(Function &F) {
    if (F.empty()) return false;

    Module &M = *F.getParent();
    auto &SBI = getAnalysis<SeaBuiltinsInfoWrapperPass>().getSBI();
    auto *assertFn = SBI.mkSeaBuiltinFn(SeaBuiltinsOp::UPRED_ASSERT, M);

    std::vector<Instruction *> workList;
    for (auto &B : F) {
      bool match = false;

      for (auto &I : B) {
        if (match) {
          match = false;
          workList.push_back(&I);
        }

        CallInst *CI = dyn_cast<CallInst>(&I);
        if (!CI) continue;

        Function* CF = CI->getCalledFunction();
        if (!CF) continue;

        match = CF->getName().equals(assertFn->getName());
      }
    }

    if (workList.empty()) return false;

    while (!workList.empty())  {
      Instruction *I = workList.back();
      workList.pop_back();
      llvm::SplitBlock(I->getParent(), I, nullptr, nullptr);
    }

    return true;
  }

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<SeaBuiltinsInfoWrapperPass>();
    AU.setPreservesAll();
  }
};

char EndBlockOnUpredAssert::ID = 0;
  
Pass *createEndBlockOnUpredAssertPass() {
  return new EndBlockOnUpredAssert();
}

} // namespace seahorn
