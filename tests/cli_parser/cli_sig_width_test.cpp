/**
 * @file tests/cli_parser/cli_sig_width_test.cpp
 * @brief The bound on how much work one signature decode may do.
 *
 * kMaxTypeDepth stops the descent at 64 levels, and a Type may contain more
 * than one Type: ELEMENT_TYPE_GENERICINST (II.23.2.12) carries a compressed
 * argument count and then that many of them. So a TypeSpec row whose own
 * signature is a GENERICINST with two self-referencing arguments branches twice
 * per level, and 64 levels of that is 2^65 nodes. The depth bound never trips,
 * because the cycle never gets deeper than 64.
 *
 * The cycle is the one the decoder already documents: the CLASS arm calls
 * tokenName, tokenName asks the resolver for a TypeSpec row's type, and
 * CLIReader::typeSpecType fetches that row's blob and decodes it with this same
 * decoder. CyclingResolver below is that, modelled.
 *
 * Measured before the work budget existed: 2,000,000 resolver calls in 0.69 s,
 * still going, from nine bytes. After: exactly 100000, in 40 ms.
 *
 * The resolver keeps a cap of its own, five million, so that a regression here
 * fails this test after about two seconds instead of never returning. A test
 * that hangs is not a test anyone can run.
 */

#include "retdec/cli_parser/cli_sig.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace retdec {
namespace cli_parser {
namespace tests {
namespace {

/// Stands in for CLIReader::typeSpecType: every TypeSpec row resolves by
/// decoding the same self-referencing blob again.
class CyclingResolver : public ITypeNameResolver {
public:
	/// Far above the decoder's own budget, and low enough to fail rather than
	/// hang if that budget ever stops being applied.
	static constexpr unsigned long long kOwnCap = 5000000;

	mutable unsigned long long calls = 0;
	std::vector<std::uint8_t> blob;
	const CliSigDecoder* decoder = nullptr;

	std::string typeDefName(std::uint32_t) const override
	{
		return "T";
	}

	std::string typeRefName(std::uint32_t) const override
	{
		return "T";
	}

	BcType typeSpecType(std::uint32_t) const override
	{
		if (++calls > kOwnCap)
		{
			return BcType{};
		}

		if (decoder)
		{
			(void)decoder->decodeField(std::span<const std::uint8_t>(blob.data(), blob.size()));
		}

		return BcType{};
	}
};

/// 06 FIELD, 15 GENERICINST, 12 06 CLASS TypeSpec row 1 -- the row itself --
/// 02 two arguments, each 12 06 the row itself again.
TEST(CliSigWidthTests, ASelfReferencingGenericInstTerminates)
{
	CyclingResolver resolver;
	resolver.blob = {0x06, 0x15, 0x12, 0x06, 0x02, 0x12, 0x06, 0x12, 0x06};

	CliSigDecoder decoder(&resolver);
	resolver.decoder = &decoder;

	(void)decoder.decodeField(std::span<const std::uint8_t>(resolver.blob.data(), resolver.blob.size()));

	EXPECT_LT(resolver.calls, CyclingResolver::kOwnCap)
		<< "the decode was still descending when the resolver stopped it; the "
		   "work budget in cli_sig.cpp is not bounding the width";
}

/// The single-argument shape is the one the depth bound already handled: it
/// does not branch, so it is 64 nodes rather than 2^65. Kept because it is the
/// control -- if this one ever stops terminating, the defect is in the depth
/// bound and not in the width one.
TEST(CliSigWidthTests, ASelfReferencingClassTerminates)
{
	CyclingResolver resolver;
	resolver.blob = {0x06, 0x12, 0x06};

	CliSigDecoder decoder(&resolver);
	resolver.decoder = &decoder;

	(void)decoder.decodeField(std::span<const std::uint8_t>(resolver.blob.data(), resolver.blob.size()));

	EXPECT_LT(resolver.calls, CyclingResolver::kOwnCap);
}

/// A budget that refused ordinary work would be worse than the hang. This is
/// the shape a language compiler emits -- Dictionary<string, List<int[]>> --
/// with a resolver that answers rather than recursing.
TEST(CliSigWidthTests, AnOrdinaryNestedGenericIsStillDecoded)
{
	class PlainResolver : public ITypeNameResolver {
	public:
		std::string typeDefName(std::uint32_t) const override
		{
			return "T";
		}
		std::string typeRefName(std::uint32_t) const override
		{
			return "T";
		}
		BcType typeSpecType(std::uint32_t) const override
		{
			return BcType{};
		}
	};

	PlainResolver resolver;
	CliSigDecoder decoder(&resolver);

	// FIELD, GENERICINST, CLASS TypeRef 1, two args: string, and a
	// GENERICINST CLASS TypeRef 1 of one arg, SZARRAY of I4.
	const std::vector<std::uint8_t> blob = {0x06, 0x15, 0x12, 0x05, 0x02, 0x0E, 0x15, 0x12, 0x05, 0x01, 0x1D, 0x08};

	auto type = decoder.decodeField(std::span<const std::uint8_t>(blob.data(), blob.size()));

	EXPECT_TRUE(type.has_value());
}

} // namespace
} // namespace tests
} // namespace cli_parser
} // namespace retdec
