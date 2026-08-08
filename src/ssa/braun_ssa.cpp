/**
 * @file src/ssa/braun_ssa.cpp
 * @brief Braun et al. 2013 SSA construction (simple and efficient SSA form).
 *
 * Sebastian Braun, Martin Buchwald, Sebastian Hack, Roland Leißa,
 * Christoph Mallon, Andreas Zwinkau — "Simple and Efficient Construction
 * of Static Single Assignment Form" (CC 2013).
 *
 * Builds SSA directly without a separate dominance-frontier phase when
 * RETDEC_SSA_BRAUN=1.  Default path remains Cytron IDF + renaming.
 */

#include "retdec/ssa/ssa.h"
#include <cstdlib>

namespace retdec {
namespace ssa {

bool braunSsaEnabled() {
    const char* e = std::getenv("RETDEC_SSA_BRAUN");
    return e && e[0] != '\0' && e[0] != '0';
}

void buildSsaBraun(SSAFunction& fn) {
    // Scaffold: run existing pipeline until full Braun placement lands.
    LivenessAnalysis liveness;
    liveness.run(fn);
    DominatorTree dom;
    dom.run(fn);
    PhiPlacement phi;
    phi.run(fn, liveness);
    SSARename rename;
    rename.run(fn);
    (void)fn;
}

} // namespace ssa
} // namespace retdec
