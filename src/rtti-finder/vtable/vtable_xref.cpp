/**
* @file src/rtti-finder/vtable/vtable_xref.cpp
* @brief Cross-reference vtable entries with demangled RTTI names.
* @copyright (c) 2024, MIT license
*
* The existing vtable_finder.cpp locates vtable structures and the RTTI
* parsers extract class names — but they don't communicate.  As a result:
*
*   - Virtual function pointers in a vtable are left anonymous.
*   - Base-class relationships discovered in RTTI are not wired back to
*     the vtable, so multi-inheritance vtables remain opaque.
*   - The demangler is never called on the RTTI names, so output shows
*     mangled symbols instead of readable class names.
*
* This file adds:
*
*  1. demangle_rtti_names()   — runs the demangler on every RTTI name found
*                               by the GCC/MSVC parsers and writes the result
*                               back into the TypeInfo::name field.
*
*  2. xref_vtables_gcc()      — for each GCC vtable, looks up the linked RTTI
*                               entry, populates VtableGcc::rtti, and annotates
*                               each virtual function slot with a generated name
*                               derived from the class name + slot index
*                               (e.g. "Foo::vfunc_2").
*
*  3. xref_vtables_msvc()     — same for MSVC vtables.
*
*  4. build_class_hierarchy() — uses the SiClassTypeInfo / VmiClassTypeInfo
*                               base-class lists to reconstruct the C++
*                               inheritance graph and record it in a simple
*                               adjacency structure for use by downstream passes.
*
* Call order (add to the end of findGccVtables / findMsvcVtables in
* vtable_finder.cpp):
*
*   demangle_rtti_names(rttis);
*   xref_vtables_gcc(vtables, rttis);
*   ClassHierarchy hier = build_class_hierarchy_gcc(rttis);
*/

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "retdec/rtti-finder/vtable/vtable_xref.h"
#include "retdec/rtti-finder/rtti/rtti_gcc.h"
#include "retdec/rtti-finder/rtti/rtti_msvc.h"
#include "retdec/rtti-finder/vtable/vtable_gcc.h"
#include "retdec/rtti-finder/vtable/vtable_msvc.h"
#include "retdec/demangler/demangler_base.h"
#include "retdec/demangler/borland_demangler.h"
#include "retdec/demangler/itanium_demangler.h"
#include "retdec/demangler/microsoft_demangler.h"
#include "retdec/utils/io/log.h"

using namespace retdec::utils::io;

namespace retdec {
namespace rtti_finder {

//===========================================================================
// 1. Demangle RTTI names
//===========================================================================

/// Attempt to demangle @a mangled using Itanium then MSVC demangler.
/// Returns the demangled name on success, or @a mangled unchanged.
static std::string tryDemangle(const std::string& mangled) {
    // GCC/Clang mangled names start with '_Z' (Itanium ABI).
    if (mangled.size() >= 2 && mangled[0] == '_' && mangled[1] == 'Z') {
        demangler::ItaniumDemangler itanium;
        std::string result = itanium.demangleToString(mangled);
        if (!result.empty() && result != mangled) return result;
    }

    // MSVC mangled names start with '?' or '_'.
    if (!mangled.empty() && (mangled[0] == '?' || mangled[0] == '_')) {
        demangler::MicrosoftDemangler ms;
        std::string result = ms.demangleToString(mangled);
        if (!result.empty() && result != mangled) return result;
    }

    // Borland fallback.
    {
        demangler::BorlandDemangler borland;
        std::string result = borland.demangleToString(mangled);
        if (!result.empty() && result != mangled) return result;
    }

    return mangled;
}

void demangle_rtti_names(RttiGcc& rttis) {
    for (auto& [addr, rtti] : rttis) {
        if (!rtti) continue;
        std::string demangled = tryDemangle(rtti->name);
        if (demangled != rtti->name) {
            Log::info() << "[VtableXref] GCC RTTI " << rtti->name
                        << " → " << demangled << "\n";
            rtti->name = demangled;
        }
    }
}

void demangle_rtti_names_msvc(RttiMsvc& rttis) {
    for (auto& [addr, rtti] : rttis) {
        if (rtti.typeName.empty()) continue;
        std::string demangled = tryDemangle(rtti.typeName);
        if (demangled != rtti.typeName) {
            Log::info() << "[VtableXref] MSVC RTTI " << rtti.typeName
                        << " → " << demangled << "\n";
            rtti.typeName = demangled;
        }
    }
}

//===========================================================================
// 2. Cross-reference GCC vtables ↔ RTTI
//===========================================================================

void xref_vtables_gcc(VtablesGcc& vtables, const RttiGcc& rttis) {
    for (auto& [vtAddr, vt] : vtables) {
        // The vtable stores the RTTI address one slot before the vtable
        // pointer; this is already parsed into VtableGcc::rttiAddress.
        if (!vt.rttiAddress.isDefined()) continue;

        auto it = rttis.find(vt.rttiAddress);
        if (it == rttis.end() || !it->second) continue;

        vt.rtti = it->second;
        const std::string& className = it->second->name;

        // Annotate virtual function slots with generated names.
        for (std::size_t i = 0; i < vt.virtualFunctions.size(); ++i) {
            auto& vfunc = vt.virtualFunctions[i];
            if (vfunc.name.empty()) {
                std::ostringstream oss;
                oss << className << "::vfunc_" << i;
                vfunc.name = oss.str();
            }
        }

        Log::info() << "[VtableXref] Vtable @" << vtAddr
                    << " → class " << className
                    << " (" << vt.virtualFunctions.size() << " vfuncs)\n";
    }
}

//===========================================================================
// 3. Cross-reference MSVC vtables ↔ RTTI
//===========================================================================

void xref_vtables_msvc(VtablesMsvc& vtables, const RttiMsvc& rttis) {
    for (auto& [vtAddr, vt] : vtables) {
        // MSVC vtable: the slot at [-1] is the address of the
        // CompleteObjectLocator, which links to the TypeDescriptor.
        // The MSVC RTTI parser already resolved this into RttiMsvc keyed
        // by the TypeDescriptor address.  We match via the COL address
        // stored in the vtable.
        if (!vt.colAddress.isDefined()) continue;

        // Look up via the COL's typeDescriptorAddress.
        for (auto& [rttiAddr, rtti] : rttis) {
            if (rtti.colAddress != vt.colAddress) continue;

            const std::string& className = rtti.typeName;
            for (std::size_t i = 0; i < vt.virtualFunctions.size(); ++i) {
                auto& vfunc = vt.virtualFunctions[i];
                if (vfunc.name.empty()) {
                    std::ostringstream oss;
                    oss << className << "::vfunc_" << i;
                    vfunc.name = oss.str();
                }
            }

            Log::info() << "[VtableXref] MSVC vtable @" << vtAddr
                        << " → class " << className << "\n";
            break;
        }
    }
}

//===========================================================================
// 4. Build class hierarchy from RTTI
//===========================================================================

ClassHierarchy build_class_hierarchy_gcc(const RttiGcc& rttis) {
    ClassHierarchy hier;

    for (auto& [addr, rtti] : rttis) {
        if (!rtti) continue;
        const std::string& name = rtti->name;
        hier.classes.insert(name);

        // SiClassTypeInfo: single inheritance — one direct base.
        if (auto* si = dynamic_cast<const SiClassTypeInfo*>(rtti.get())) {
            if (si->baseClass) {
                hier.bases[name].push_back(si->baseClass->name);
                hier.derived[si->baseClass->name].push_back(name);
            }
        }
        // VmiClassTypeInfo: multiple/virtual inheritance.
        else if (auto* vmi = dynamic_cast<const VmiClassTypeInfo*>(rtti.get())) {
            for (const auto& base : vmi->baseClasses) {
                if (base.baseClass) {
                    hier.bases[name].push_back(base.baseClass->name);
                    hier.derived[base.baseClass->name].push_back(name);
                }
            }
        }
    }

    // Log summary.
    Log::info() << "[VtableXref] Class hierarchy: "
                << hier.classes.size() << " classes, "
                << hier.bases.size() << " with base classes\n";

    return hier;
}

ClassHierarchy build_class_hierarchy_msvc(const RttiMsvc& rttis) {
    ClassHierarchy hier;

    for (auto& [addr, rtti] : rttis) {
        const std::string& name = rtti.typeName;
        if (name.empty()) continue;
        hier.classes.insert(name);

        for (const auto& base : rtti.baseClasses) {
            if (base.typeName.empty()) continue;
            hier.bases[name].push_back(base.typeName);
            hier.derived[base.typeName].push_back(name);
        }
    }

    return hier;
}

} // namespace rtti_finder
} // namespace retdec
