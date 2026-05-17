/**
 * @file include/retdec/fileformat/types/import_table/elf_import_table.h
 * @brief Class for ELF import table.
 * @copyright (c) 2021 Odin Loch Trading as Imortek
 */

#include "import_table.h"
#include "retdec/fileformat/types/dotnet_headers/metadata_tables.h"

namespace retdec {
namespace fileformat {

class ElfImportTable : public ImportTable
{
public:
	void computeHashes() override;
};

} // namespace fileformat
} // namespace retdec
