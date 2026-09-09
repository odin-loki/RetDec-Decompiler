/**
 * @file src/eh_reconstruct/lsda_parser.h
 * @brief Shared LSDA (Language Specific Data Area) parser.
 *
 * Both the Itanium .eh_frame parser and the ARM EHABI parser use Itanium-format
 * LSDA data to describe call-site → landing-pad → action-chain mappings.
 * This header exposes `parseLSDA` and `lsdaToTryCatch` so that both parsers
 * can share the same implementation without duplication.
 *
 * DW_EH_PE constants and LsdaResult are intentionally kept in this header so
 * both consumers can include only this file.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "retdec/eh_reconstruct/eh_reconstruct.h"

namespace retdec {
namespace eh_reconstruct {

// ─── DW_EH_PE pointer-encoding constants ─────────────────────────────────────

static constexpr uint8_t DW_EH_PE_omit     = 0xff;
static constexpr uint8_t DW_EH_PE_absptr   = 0x00;
static constexpr uint8_t DW_EH_PE_uleb128  = 0x01;
static constexpr uint8_t DW_EH_PE_udata2   = 0x02;
static constexpr uint8_t DW_EH_PE_udata4   = 0x03;
static constexpr uint8_t DW_EH_PE_udata8   = 0x04;
static constexpr uint8_t DW_EH_PE_sleb128  = 0x09;
static constexpr uint8_t DW_EH_PE_sdata2   = 0x0a;
static constexpr uint8_t DW_EH_PE_sdata4   = 0x0b;
static constexpr uint8_t DW_EH_PE_sdata8   = 0x0c;
static constexpr uint8_t DW_EH_PE_pcrel    = 0x10;
static constexpr uint8_t DW_EH_PE_textrel  = 0x20;
static constexpr uint8_t DW_EH_PE_datarel  = 0x30;
static constexpr uint8_t DW_EH_PE_funcrel  = 0x40;
static constexpr uint8_t DW_EH_PE_indirect = 0x80;

// ─── LSDA parse result ────────────────────────────────────────────────────────

struct LsdaResult {
    struct Site {
        uint64_t  csStart    = 0;  ///< Start VMA of the guarded region
        uint64_t  csEnd      = 0;  ///< End VMA (exclusive)
        uint64_t  landingPad = 0;  ///< Landing pad VMA (0 = none / CANTUNWIND)
        uint32_t  action     = 0;  ///< 0 = cleanup, else 1-based action index
    };
    struct Action {
        int64_t  typeFilter = 0;   ///< >0 type-table index; 0 catch-all; <0 exception-spec
        int64_t  nextOffset = 0;   ///< byte offset to next action (0 = end of chain)
		uint64_t recordVma = 0;    ///< VMA this record was read from
		/// VMA of the next record in the chain, 0 at the end.
		///
		/// ar_next is a byte offset from immediately after itself, so the
		/// successor cannot be found by stepping to the next element of
		/// `actions`; parseLSDA computes this address as it walks and records
		/// it here, and `actionByVma` turns it back into an index.
		uint64_t nextVma = 0;
	};
    std::vector<Site>     sites;
    std::vector<Action>   actions;
    std::vector<uint64_t> typeTable; ///< type_info VMAs (1-based; index 0 unused)

	/// VMA of an action record → its index in `actions`.
	///
	/// Site::action is the 1-based *byte offset* of the first action record
	/// within the action table -- parseLSDA treats it that way itself -- while
	/// `actions` is ordered by discovery. lsdaToTryCatch used the offset as a
	/// vector index and walked the chain with ++index instead of following
	/// nextOffset; the two agree only when every record is exactly two bytes
	/// and the chain starts at offset 0. Any other layout indexed the wrong
	/// action or fell off the end, and because the site clears its block's
	/// handlers first, an out-of-range index left the block empty and it was
	/// dropped. This map is how a site finds its chain.
	std::map<uint64_t, std::size_t> actionByVma;
	uint64_t actionTableBase = 0; ///< VMA the 1-based Site::action counts from
};

// ─── Public API ───────────────────────────────────────────────────────────────

/**
 * @brief Parse a single LSDA blob at `lsdaVma`.
 *
 * @param view       Binary memory view (used for reading pointer-encoded data).
 * @param lsdaVma    VMA where the LSDA starts.
 * @param funcStart  VMA of the function whose LSDA this is (used as lpstart
 *                   when lpstart_enc is DW_EH_PE_omit).
 * @return LsdaResult populated with sites, actions, and type-table entries.
 */
LsdaResult parseLSDA(const IBinaryView& view,
                     uint64_t lsdaVma,
                     uint64_t funcStart);

/**
 * @brief Translate a parsed LsdaResult into try/catch structure on `fn`.
 *
 * Groups call-site records by landing pad, then walks action chains to
 * populate EHFunction::tryCatchBlocks.
 */
void lsdaToTryCatch(const IBinaryView& view,
                    const LsdaResult& lsda,
                    EHFunction& fn);

} // namespace eh_reconstruct
} // namespace retdec
