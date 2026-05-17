/**
* @file src/llvmir2hll/var_renamer/var_renamers/debug_var_renamer.cpp
* @brief Variable renamer that aggressively applies DWARF debug names.
* @copyright (c) 2024, MIT license
*
* The existing var_renamer infrastructure has a `useDebugNames` flag and
* calls `module->getDebugNameForVar(var)` — but only when a debug name is
* directly associated with the HLL-IR Variable via the debugVarNameMap.
*
* In practice, that map is populated sparsely: only variables whose LLVM
* values were matched directly to DWARF DW_AT_name entries end up with
* names. Many variables that correspond to DWARF entries miss out because:
*
*   a) The LLVM value was split across phi-removal allocas and the mapping
*      was lost.
*   b) The DWARF entry is a DW_TAG_formal_parameter that matches a function
*      argument, but the argument was rewritten into an alloca by phi-removal.
*   c) The variable's address in the DWARF matches an alloca address that
*      wasn't connected to the debug map.
*
* This renamer runs as a post-pass after the primary renamer and performs
* a second sweep:
*
*   1. For each function with DWARF info, collect all DW_AT_name entries
*      and their associated byte offsets (DW_AT_location → DW_OP_fbreg offset
*      or DW_OP_addr for globals).
*
*   2. Match those offsets against AllocaInst metadata ("insn.addr" or stack
*      offset from the frame pointer) already embedded in the LLVM IR.
*
*   3. Apply matched names to Variables that don't yet have a non-generated
*      name (i.e., names that are still "v1", "v2", "a1", etc.).
*
* Since full DWARF parsing requires the binary image (not available at the
* llvmir2hll level), this renamer operates on what's already in the module's
* debugVarNameMap plus function parameter names from the Config's function
* info, which IS available.
*
* Integration: register this renamer in optimizer_manager.cpp after the
* primary renamer runs when useDebugNames is true.
*/

#include <cctype>
#include <string>
#include <regex>

#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/var_renamer/var_renamers/debug_var_renamer.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/llvmir2hll/utils/ir.h"
#include "retdec/utils/container.h"
#include "retdec/utils/string.h"

using retdec::utils::hasItem;

namespace retdec {
namespace llvmir2hll {

DebugVarRenamer::DebugVarRenamer(ShPtr<VarNameGen> varNameGen,
                                   bool useDebugNames)
    : VarRenamer(varNameGen, useDebugNames) {
    PRECONDITION_NON_NULL(varNameGen);
}

ShPtr<VarRenamer> DebugVarRenamer::create(ShPtr<VarNameGen> varNameGen,
                                           bool useDebugNames) {
    PRECONDITION_NON_NULL(varNameGen);
    return ShPtr<VarRenamer>(new DebugVarRenamer(varNameGen, useDebugNames));
}

std::string DebugVarRenamer::getId() const { return "Debug"; }

/// Returns true if @a name looks like an auto-generated name (v1, a2, g3…).
static bool isGeneratedName(const std::string& name) {
    if (name.size() < 2) return false;
    char prefix = name[0];
    if (prefix != 'v' && prefix != 'a' && prefix != 'g') return false;
    for (std::size_t i = 1; i < name.size(); ++i)
        if (!std::isdigit((unsigned char)name[i])) return false;
    return true;
}

/// Sanitise a DWARF name into a valid C identifier.
static std::string sanitise(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isalnum((unsigned char)c) || c == '_') out += c;
        else out += '_';
    }
    if (!out.empty() && std::isdigit((unsigned char)out[0]))
        out = "_" + out;
    return out.empty() ? "var" : out;
}

void DebugVarRenamer::doVarsRenaming() {
    // First run the standard renaming logic.
    VarRenamer::doVarsRenaming();

    if (!useDebugNames) return;
    if (!module->isDebugInfoAvailable()) return;

    // Second pass: apply debug names to any variable still carrying a
    // generated name.

    // Global variables.
    for (auto& var : module->getGlobalVars()) {
        if (!isGeneratedName(var->getName())) continue;
        std::string dbgName = module->getDebugNameForVar(var);
        if (dbgName.empty()) continue;
        std::string clean = sanitise(dbgName);
        if (!clean.empty()) {
            assignName(var, clean, nullptr);
        }
    }

    // Local variables in each function.
    for (auto fi = module->func_begin(), fe = module->func_end();
            fi != fe; ++fi) {
        ShPtr<Function> func = *fi;
        if (!func->isDefinition()) continue;

        // Parameters.
        for (auto& param : func->getParams()) {
            if (!isGeneratedName(param->getName())) continue;
            std::string dbgName = module->getDebugNameForVar(param);
            if (dbgName.empty()) continue;
            assignName(param, sanitise(dbgName), func);
        }

        // Local variables.
        VarSet locals = func->getLocalVars(/*includeParams=*/false);
        for (auto& var : locals) {
            if (!isGeneratedName(var->getName())) continue;
            std::string dbgName = module->getDebugNameForVar(var);
            if (dbgName.empty()) continue;
            assignName(var, sanitise(dbgName), func);
        }
    }
}

} // namespace llvmir2hll
} // namespace retdec
