/**
 * @file include/retdec/fileformat/types/dotnet_headers/user_string_stream.h
 * @brief Class for \#US Stream.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_FILEFORMAT_TYPES_DOTNET_HEADERS_USER_STRING_STREAM_H
#define RETDEC_FILEFORMAT_TYPES_DOTNET_HEADERS_USER_STRING_STREAM_H

#include "retdec/fileformat/types/dotnet_headers/stream.h"

namespace retdec {
namespace fileformat {

class UserStringStream : public Stream
{
	public:
		UserStringStream(std::uint64_t streamOffset, std::uint64_t streamSize);
};

} // namespace fileformat
} // namespace retdec

#endif
