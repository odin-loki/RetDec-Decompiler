/**
 * @file tests/utils/equality_odr_second_tu.cpp
 * @brief The second translation unit for the ODR check in equality_tests.cpp.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * retdec/utils/equality.h used to define its floating-point helper in an
 * anonymous namespace, in a header. That gives every including translation
 * unit its own copy with internal linkage, and the `inline` areEqual<>
 * specializations -- which have external linkage and must be identical
 * everywhere -- each named their own. An ODR violation is ill-formed with no
 * diagnostic required, so no compiler will say so; the only way to observe it
 * is to ask two translation units whether they agree about the address, which
 * is what this file exists to make possible.
 */

#include "retdec/utils/equality.h"

namespace retdec {
namespace utils {
namespace tests {

const void* equalityHelperAddressFromSecondTu()
{
	return reinterpret_cast<const void*>(&detail::areEqualFPWithEpsilon<double>);
}

const void* areEqualDoubleAddressFromSecondTu()
{
	return reinterpret_cast<const void*>(static_cast<bool (*)(const double&, const double&)>(&areEqual<double>));
}

} // namespace tests
} // namespace utils
} // namespace retdec
