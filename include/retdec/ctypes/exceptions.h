/**
* @file include/retdec/ctypes/exceptions.h
* @brief Exceptions for C-types.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_CTYPES_EXCEPTIONS_H
#define RETDEC_CTYPES_EXCEPTIONS_H

#include <stdexcept>

namespace retdec {
namespace ctypes {

/**
* @brief Base class for all C-types errors.
*/
class CTypesError: public std::runtime_error
{
	public:
		using std::runtime_error::runtime_error;
};

} // namespace ctypes
} // namespace retdec

#endif
