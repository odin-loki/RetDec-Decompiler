/**
 * @file src/fileinfo/file_wrapper/macho_wrapper.h
 * @brief Definition of MachOWrapper class.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#ifndef FILEINFO_FILE_WRAPPER_MACHO_WRAPPER_H
#define FILEINFO_FILE_WRAPPER_MACHO_WRAPPER_H

#include "retdec/fileformat/file_format/macho/macho_format.h"

namespace retdec {
namespace fileinfo {

/**
 * Wrapper for parsing MachO files
 */
class MachOWrapper : public retdec::fileformat::MachOFormat
{
	public:
		MachOWrapper(std::string pathToFile, retdec::fileformat::LoadFlags loadFlags);

		/// @name Detection methods
		/// {
		const llvm::object::MachOObjectFile* getMachOParser() const;
		std::string getTypeOfFile() const;
		/// }
};

} // namespace fileinfo
} // namespace retdec

#endif
