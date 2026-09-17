// Harness stub for DebugFormat::loadDwarf.
//
// src/debugformat/dwarf.cpp does not compile against the distribution LLVM 20
// in this container: it uses the LLVM 21 DataExtractor constructor and the
// post-20 DWARF/LowLevel/ header layout. No bin2llvmir unit test loads DWARF,
// so the harness supplies an empty definition rather than the real one. A test
// that did exercise DWARF would silently see no debug info, which is why this
// is a harness file and not a source change.
#include "retdec/debugformat/debugformat.h"

namespace retdec {
namespace debugformat {

void DebugFormat::loadDwarf() {}

} // namespace debugformat
} // namespace retdec
