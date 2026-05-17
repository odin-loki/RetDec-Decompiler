/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/goto_cfg_optimizer.h
* @brief Structural elimination of spurious goto statements.
*
* Recognises the patterns that GCC compiler optimisations introduce into the
* control-flow graph and inverts them at the HLL IR level, following the
* approach of the SAILR algorithm (USENIX Security 2024).
*
* Patterns handled (applied iteratively until fixed point):
*
*  (A) Forward-skip:
*        if (cond) goto L;
*        stmts...
*        L:
*      →  if (!cond) { stmts... }
*
*  (B) Trivial forward jump (unconditional goto to immediate successor region):
*        goto L;
*        L: stmts...
*      →  stmts...   (remove the goto)
*
*  (C) Tail-goto to continue target (loop header label):
*        L_loop:
*          body;
*          goto L_loop;
*      →  while (true) { body; }
*      (relies on while_true_to_while_cond_optimizer to clean up afterwards)
*
*  (D) Goto to early return:
*        goto L;  /  if(c) goto L;
*        ...
*        L: return expr;
*      →  return expr;  /  if(c) { return expr; }
*      (already partly done by GotoStmtOptimizer; extended here)
*/

#ifndef RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_GOTO_CFG_OPTIMIZER_H
#define RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_GOTO_CFG_OPTIMIZER_H

#include "retdec/llvmir2hll/optimizer/func_optimizer.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"

namespace retdec {
namespace llvmir2hll {

class Module;

/**
* @brief Eliminates spurious gotos by inverting compiler CFG optimisations.
*
* Instances of this class have reference object semantics.
*/
class GotoCFGOptimizer final : public FuncOptimizer {
public:
    explicit GotoCFGOptimizer(ShPtr<Module> module);

    virtual std::string getId() const override { return "GotoCFG"; }

protected:
    virtual void runOnFunction(ShPtr<Function> func) override;
};

} // namespace llvmir2hll
} // namespace retdec

#endif // RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_GOTO_CFG_OPTIMIZER_H
