/**
* @file include/retdec/rtti-finder/vtable/vtable_xref.h
* @brief Cross-reference vtable entries with demangled RTTI names.
* @copyright (c) 2024, MIT license
*/

#ifndef RETDEC_RTTI_FINDER_VTABLE_VTABLE_XREF_H
#define RETDEC_RTTI_FINDER_VTABLE_VTABLE_XREF_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "retdec/rtti-finder/rtti/rtti_gcc.h"
#include "retdec/rtti-finder/rtti/rtti_msvc.h"
#include "retdec/rtti-finder/vtable/vtable_gcc.h"
#include "retdec/rtti-finder/vtable/vtable_msvc.h"

namespace retdec {
namespace rtti_finder {

/// Simple class hierarchy: class name → list of direct base class names.
struct ClassHierarchy {
    std::set<std::string>                        classes;  ///< All known class names.
    std::map<std::string, std::vector<std::string>> bases;  ///< derived → bases.
    std::map<std::string, std::vector<std::string>> derived;///< base → deriveds.
};

/// Demangle all RTTI names in @a rttis using Itanium/MSVC/Borland demanglers.
void demangle_rtti_names(RttiGcc& rttis);
void demangle_rtti_names_msvc(RttiMsvc& rttis);

/// Populate vtable::rtti links and annotate virtual function slots with names.
void xref_vtables_gcc(VtablesGcc& vtables, const RttiGcc& rttis);
void xref_vtables_msvc(VtablesMsvc& vtables, const RttiMsvc& rttis);

/// Build an inheritance graph from RTTI structures.
ClassHierarchy build_class_hierarchy_gcc(const RttiGcc& rttis);
ClassHierarchy build_class_hierarchy_msvc(const RttiMsvc& rttis);

} // namespace rtti_finder
} // namespace retdec

#endif // RETDEC_RTTI_FINDER_VTABLE_VTABLE_XREF_H
