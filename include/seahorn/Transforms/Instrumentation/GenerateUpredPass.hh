#ifndef __PROMOTE_UPRED_PASS__HH_
#define __PROMOTE_UPRED_PASS__HH_

/**
 * Locates external function calls annotated by "upred" and generates
 * placeholder implementations.
 */

#include "llvm/IR/Function.h"
#include "llvm/Pass.h"

#include <set>
#include <string>

using namespace llvm;

namespace seahorn {

class SeaBuiltinsInfo;

class GenerateUpredPass : public llvm::ModulePass {
private:
  static const std::string PREFIX;

  Function *m_assumeFn;

public:
  static char ID;

  GenerateUpredPass();

  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const;

  bool runOnModule(Module &M);
  bool runOnFunction(SeaBuiltinsInfo &SBI, Function &F);

  virtual StringRef getPassName() const { return "PromoteUpred"; }

  // Returns true if this pass has marked F as an uninterpreted predicate.
  static bool isUpred(const Function &F);
};

} // namespace seahorn

#endif /* __PROMOTE_UPRED_PASS__HH_ */
