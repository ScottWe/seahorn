#include "seahorn/Analysis/ApiAnalysisPass.hh"

/**
* Identifies functions that call a specific API function
*/
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Support/raw_ostream.h"
#include "avy/AvyDebug.h"
#include "llvm/ADT/SCCIterator.h"
#include "boost/range/algorithm/reverse.hpp"

namespace seahorn
{
  using namespace llvm;

  void ApiAnalysisPass::analyze(const Function *F, unsigned int& progress, ApiCallInfo& aci)
  {
    for (auto analyzedfunc : m_apiAnalysis)
    {
      if (F->getName() == analyzedfunc.m_func->getName())
      {
        if (analyzedfunc.getFinalAnalysis().m_progress == m_apilist.size())
        {
          return;
        }
      }
    }

    //outs() << "In function: " << F->getName() << "\n";

    // First, get the basic blocks in topological order
    std::vector<const BasicBlock*> sortedBBlocks;
    RevTopoSort(*F,sortedBBlocks);
    boost::reverse(sortedBBlocks);

    // Required API calls are initialized for this BB
    BBApiList bblist;
    unsigned int initProgress = progress;

    while ( progress < m_apilist.size())
    {
      std::string API = m_apilist.at(progress);

      //outs() << "Looking for " << API << ", progress = " << progress << "\n";

      // It appears that two calls can occur in an LLLVM basic block? So each block
      // must be processed again and again?

      unsigned int apiIncrement=0;

      for (const BasicBlock *bb : sortedBBlocks)
      {
        // determine if the API is called
        for (BasicBlock::const_iterator bi = bb->begin(); bi != bb->end(); bi++)
        {
          const Instruction *I = &*bi;
          if (const CallInst *CI = dyn_cast<CallInst> (I)) {

            CallSite CS (const_cast<CallInst*> (CI));
            const Function *cf = CS.getCalledFunction();

            // this block contains an API function call of interest
            if (cf)
            {
              if (cf->getName().str() == API)
              {

                apiIncrement++;

                // outs() << "API " << API << " found in ";
                // bb->printAsOperand(outs(), false);
                // outs() << ", increment: " << apiIncrement << ", progress: " << progress << "\n";

                if ( (progress+apiIncrement) < m_apilist.size())
                {
                  API = m_apilist.at(progress+apiIncrement); // go to the next API

                }
                else
                {
                  break;
                }
              }
              else
              {
                // handle external calls
                if (!cf->empty())
                {

                  // handle function calls
                  // outs() << "In function "<< F->getName() << " calling outgoing function "
                  // << cf->getName() << " looking for "
                  // << API << "\n";

                  analyze(cf, progress, aci);
                }
              }
            }
          }
        }

        // outs() << "API Increment: " << apiIncrement << "\n";
        // found the API

        // get the predecessor and propagate analysis info
        unsigned int max_progress = progress;
        for (auto it = pred_begin(bb), et = pred_end(bb); it != et; ++it)
        {
          const BasicBlock* predBB = *it;

          // outs() << "Predecessor BB: ";
          // predBB->printAsOperand(outs(), false);
          // outs() << "\n";

          for (auto bli = bblist.begin(),ble=bblist.end(); bli!=ble; bli++)
          {
            const BasicBlock* processedBB = bli->m_bb; //bli->first;

            if (processedBB == predBB) // found a direct predecessor
            {
              unsigned int prev_progress = bli->m_progress; //bli->second;

              //outs() << "Prev progress was " << prev_progress << "\n";

              if (prev_progress > max_progress)
              {
                max_progress = prev_progress;
              }
            }
          }
        }

        // Now know the progress value, save it

        ApiEntry bbentry;
        bbentry.m_bb = bb;
        bbentry.m_progress = max_progress+apiIncrement;
        bbentry.m_func = F->getName();

        progress += apiIncrement; // go to next API(s)

        apiIncrement=0;

        aci.m_bblist.push_back(bbentry);
      }

      // match not here
      if (initProgress == progress ) break;

    }
  }

  //
  // Report search results
  void ApiAnalysisPass::report()
  {
    for (auto& analysis : m_apiAnalysis)
    {
      if (!analysis.m_bblist.empty())
      {
        ApiEntry final = analysis.getFinalAnalysis();
        if (final.m_progress == m_apilist.size())
        {
          outs () << analysis.m_func->getName() << "\n";

          for (auto bentry : analysis.m_bblist)
          {
            outs() << "\t";
            bentry.m_bb->printAsOperand(outs(), false);
            outs() << " : " <<  bentry.m_progress << " (" << bentry.m_func<< ")\n";
          }
          outs() << "\t---\n" << final.m_bb->getName() << ": " << final.m_progress << "\n";

          outs() << "\nMATCH!\n";

          std::vector<const Function *> chain;
          chain.push_back(analysis.m_func);
          findStartingPoints(analysis.m_func);
        }
      }
    }
    errs() << "\nPossible starting points for this sequence: ";

    for (size_t i=0; i < m_startingPoints.size();i++)
    {
      errs() << m_startingPoints[i]->getName();
      if (i+1<m_startingPoints.size())
      {
        errs() << ", ";
      }
    }
    errs() << "\n";
  }

  void ApiAnalysisPass::findStartingPoints(const Function* F)
  {
    if (F->getNumUses() == 0)
    {
      if (std::find (m_startingPoints.begin(), m_startingPoints.end(),F) == m_startingPoints.end())
      {
        m_startingPoints.push_back(F);
      }
    }
    else
    {
      for (auto U : F->users())
      {
        if (isa<CallInst> (U) || isa<InvokeInst> (U))
        {
          if (const Instruction *Inst = dyn_cast<Instruction>(U))
          {
            // Get The grandparent of an instruction is a function (Inst -> BB -> Function)
            const Function * callingFunc = Inst->getParent()->getParent();
            findStartingPoints(callingFunc);

          }
        }
      }
    }
  }

  // Parses the sequence of APIs to search for
  void ApiAnalysisPass::parseApiString(std::string apistring)
  {
    std::istringstream ss(apistring);
    std::string api;
    while(std::getline(ss, api, ','))
    {
      m_apilist.push_back(api);
    }
  }

  // The body of the pass
  bool ApiAnalysisPass::runOnModule (Module &M)
  {
    std::vector<Function*> sortedFuncs;
    CallGraph &CG = getAnalysis<CallGraphWrapperPass> ().getCallGraph();

    for (auto it = scc_begin (&CG); !it.isAtEnd (); ++it)
    {
      auto &scc = *it;
      for (CallGraphNode *cgn : scc)
      {
        Function *f = cgn->getFunction();
        if (!f) continue;
        sortedFuncs.push_back(f);
      }
    }

    // This call generates API call information for each
    unsigned int progress;
    for (Function *F : sortedFuncs)
    {
      progress=0;
      ApiCallInfo aci;

      aci.m_func = F;
      analyze(F,progress,aci);

      //outs() << "Adding ACI " << F->getName() << "\n";
      m_apiAnalysis.push_back(aci);
    }

    report();

    return false;
  }

  void ApiAnalysisPass::getAnalysisUsage (AnalysisUsage &AU) const {
    AU.setPreservesAll ();
    AU.addRequired<CallGraphWrapperPass> ();
    AU.addPreserved<CallGraphWrapperPass> ();
  }

  char ApiAnalysisPass::ID = 0;

  llvm::Pass *createApiAnalysisPass(std::string &config) {
    return new ApiAnalysisPass(config);
  }

  llvm::Pass *createApiAnalysisPass() {
    return new ApiAnalysisPass();
  }
}   // namespace seahorn

static llvm::RegisterPass<seahorn::ApiAnalysisPass>
X("call-api", "Determine if a given API is called",false, false);
