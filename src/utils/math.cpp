/**
* @file src/utils/math.cpp
* @brief Mathematical utilities.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include "retdec/utils/math.h"

namespace retdec {
namespace utils {

/**
* @brief Counts all 1 bits in the given number.
*/
unsigned countBits(unsigned long long n) {
	unsigned count = 0;
	for (count = 0; n != 0; n &= n - 1) {
		++count;
	}
	return count;
}

/**
* @brief Returns the number of bits needed to encode the given number.
*/
unsigned bitSizeOfNumber(unsigned long long v) {
	unsigned int r = 0;
	while (v >>= 1) {
		r++;
	}
	return r + 1;
}

} // namespace utils
} // namespace retdec
