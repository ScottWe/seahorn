#include "seahorn/Transforms/Instrumentation/GenerateUpredPass.hh"

#include "seahorn/Passes.hh"
#include "seahorn/InitializePasses.hh"
#include "seahorn/Analysis/SeaBuiltinsInfo.hh"
#include "seahorn/Support/SeaDebug.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm_seahorn/IR/LLVMContext.h"

#define UPRED_ANNOTATION "upred"

using namespace llvm;

namespace seahorn {

const std::string GenerateUpredPass::PREFIX("upred.inferable.");

GenerateUpredPass::GenerateUpredPass(): ModulePass(ID) {}

void GenerateUpredPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<SeaBuiltinsInfoWrapperPass>();
}

bool GenerateUpredPass::runOnModule(Module &M) {
  auto &SBI = getAnalysis<SeaBuiltinsInfoWrapperPass>().getSBI();
  m_assumeFn = SBI.mkSeaBuiltinFn(SeaBuiltinsOp::ASSUME, M);

  bool changed = false;
  for (auto &F : M) {
    if (!F.empty()) {
      changed |= runOnFunction(SBI, F);
    }
  }
  return changed;
}

bool GenerateUpredPass::runOnFunction(SeaBuiltinsInfo &SBI, Function &F) {
  // Provides a placeholder implementation to each upred external call.
  bool changed = false;
  for (auto &B : F) {
    IRBuilder<> builder(&B);

    for (auto &I : B) {
      // Does I call a user defined function.
      CallBase *CI = dyn_cast<CallBase>(&I);
      if (!CI) continue;
      if (SBI.isSeaBuiltin(*CI)) continue;

      // Is the callee defined.
      Function* CF = CI->getCalledFunction();
      if (!CF) continue;
      if (!CF->isDeclaration()) continue;

      // Does the function return an integer?
      if (!CF->getReturnType()) continue;
      if (!CF->getReturnType()->isIntegerTy()) continue;

      // Is the caller annotated?
      auto *existing = I.getMetadata(llvm_seahorn::LLVMContext::MD_annotation);
      if (!existing) continue;

      // Does the caller have the upred annotation?
      bool isPred = false;
      auto *tuple = cast<MDTuple>(existing);
      for (auto &N : tuple->operands()) {
        if (cast<MDString>(N.get())->getString() == UPRED_ANNOTATION) {
          isPred = true;
          break;
        }
      }
      if (!isPred) continue;
  
      // Records changes
      changed = true;
      LOG("upred",
          errs() << "Found upred external call " << CF->getName().str()
                 << " in " << F.getName().str() << ".\n");

      // Assumes that the call never happens.
      builder.SetInsertPoint(&I);
      CallInst *ci = builder.CreateCall(m_assumeFn, builder.getFalse());

      // Generates a placeholder body.
      std::vector<Value *> forwardArgs;
      for (auto &arg : CF->args()) forwardArgs.push_back(&arg);
      CF->setName(PREFIX + CF->getName().str());
      CF->addFnAttr(Attribute::NoInline);
      CF->addFnAttr(Attribute::OptimizeNone);
      BasicBlock* block = BasicBlock::Create(CF->getContext(), "entry", CF);
      IRBuilder<> predBuilder(block);
      auto prev_rv = predBuilder.getInt32(0);
      predBuilder.CreateRet(prev_rv);
    }
  }

  return changed;
}

bool GenerateUpredPass::isUpred(const Function &F) {
  return (F.getName().str().compare(0, PREFIX.size(), PREFIX) == 0);
}

char GenerateUpredPass::ID = 0;

llvm::Pass *createGenerateUpredPass() {
  return new GenerateUpredPass();
}

} // namespace seahorn

using namespace seahorn;
INITIALIZE_PASS(GenerateUpredPass, "upred-generator",
                "Marks and instruments uninterpreted predicates.",
                false, false)
