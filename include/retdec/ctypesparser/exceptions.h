/**
* @file include/retdec/ctypesparser/exceptions.h
* @brief Exceptions for C-types parser.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_CTYPESPARSER_EXCEPTIONS_H
#define RETDEC_CTYPESPARSER_EXCEPTIONS_H

#include "retdec/ctypes/exceptions.h"

namespace retdec {
namespace ctypesparser {

/**
* @brief A class for ctypesparser-related errors.
*/
class CTypesParseError: public retdec::ctypes::CTypesError
{
	public:
		using retdec::ctypes::CTypesError::CTypesError;
};

} // namespace ctypesparser
} // namespace retdec

#endif
