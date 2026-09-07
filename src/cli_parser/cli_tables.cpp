/**
 * @file src/cli_parser/cli_tables.cpp
 * @brief ECMA-335 metadata table decoder.
 */

#include "retdec/cli_parser/cli_tables.h"

#include "retdec/utils/bounds.h"
#include "retdec/utils/index_translation.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace retdec {
namespace cli_parser {

// ─── RowReader helpers ────────────────────────────────────────────────────────

// Every read is guarded. `pos + n <= size` is the natural way to write the
// guard and is wrong once pos comes from a file-declared row count, so the
// comparison lives in bounds::rangeFits, which cannot wrap. A refused read
// yields zero and latches `truncated`: the row is meaningless either way, and
// the caller checks the flag rather than every return value.
bool MetadataTables::RowReader::has(size_t n) const
{
	return utils::bounds::rangeFits(pos, size, n);
}

uint8_t MetadataTables::RowReader::u8()
{
	if (!has(1))
	{
		truncated = true;
		return 0;
	}
	return data[pos++];
}

uint16_t MetadataTables::RowReader::u16()
{
	if (!has(2))
	{
		truncated = true;
		pos = size;
		return 0;
	}
	uint16_t v = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
	pos += 2;
	return v;
}

uint32_t MetadataTables::RowReader::u32()
{
	if (!has(4))
	{
		truncated = true;
		pos = size;
		return 0;
	}
	uint32_t v = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8)
			   | (static_cast<uint32_t>(data[pos + 2]) << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24);
	pos += 4;
	return v;
}

uint32_t MetadataTables::RowReader::strIdx()
{
	return wideStr ? u32() : u16();
}

uint32_t MetadataTables::RowReader::guidIdx()
{
	return wideGuid ? u32() : u16();
}

uint32_t MetadataTables::RowReader::blobIdx()
{
	return wideBlob ? u32() : u16();
}

uint32_t MetadataTables::RowReader::tableIdx(TableId tbl)
{
	uint32_t id = static_cast<uint32_t>(tbl);
	if (id < static_cast<uint32_t>(TableId::_Count) && rowCounts[id] > 0xFFFF) return u32();
	return u16();
}

// Coded token: tagBits low bits = tag, remaining high bits = row index
uint32_t MetadataTables::RowReader::codedIdx(const uint8_t* tableIds, size_t count, uint8_t tagBits)
{
	// Determine if we need 4 bytes: any referenced table has >= 2^(16-tagBits)
	// rows (ECMA-335 II.24.2.6 gives a coded index 16 bits, tagBits of which
	// are the tag).
	//
	// This was `uint32_t maxRows = 1u << (16 - tagBits);` with tagBits a
	// uint8_t. The subtraction is done on unsigned int, so for any tagBits
	// above 16 it wraps and the shift count becomes astronomical -- undefined
	// behaviour, not merely a wrong threshold. ESBMC discharges the line as
	// written for the widths II.24.2.6 actually defines (1..5, every current
	// caller passes a literal in that range) and refutes it as soon as tagBits
	// is unconstrained: FAILED shift-undefined-behavior at tagBits = 65. So
	// this is the line a new coded-token kind added with the wrong constant
	// would break, and nothing here would say so.
	//
	// idxmap::wideThreshold clamps instead of wrapping and computes at 64 bits,
	// so the shift is defined for every unsigned tagBits. The comparison is
	// widened to match; at tagBits == 0 the threshold is 65536, which no longer
	// has to fit the 32-bit type the old maxRows used.
	const uint64_t maxRows = utils::idxmap::wideThreshold(tagBits);
	bool wide = false;
	for (size_t i = 0; i < count && !wide; ++i)
	{
		uint8_t tid = tableIds[i];
		if (tid < static_cast<uint8_t>(TableId::_Count) && static_cast<uint64_t>(rowCounts[tid]) >= maxRows)
			wide = true;
	}
	return wide ? u32() : u16();
}

// Split a coded index into the table its tag selects and the row index above
// that tag, exactly as ECMA-335 II.24.2.6 defines it.
//
// This used to be written out inside RowReader::codedToken as
//
//     uint32_t tag = raw & ((1u << tagBits) - 1);
//     uint32_t idx = raw >> tagBits;
//
// which is the same undefined shift on the same uint8_t tagBits that codedIdx
// above was changed to avoid -- and RowReader::codedToken, which is where it
// was written, is codedIdx's only caller. The left operand of each shift is a
// 32-bit unsigned int, so a tagBits of 32 or more is undefined behaviour rather
// than a wrong threshold: g++ -fsanitize=undefined on those two expressions at
// the same tagBits = 65 that ESBMC produced for codedIdx reports "shift
// exponent 65 is too large for 32-bit type 'unsigned int'" for each. Without a
// sanitizer nothing says so -- an emitted x86-64 shift takes its count modulo
// 32, so tagBits = 65 masks and shifts by 1, and constant folding gives a
// different wrong answer again -- and either way a token comes back naming a
// table and a row for a width that has no meaning.
//
// idxmap::splitTag is the same split with that width refused, and it takes the
// mask from leb128::maskFrom rather than from a second shift, so the mask and
// the shift cannot drift apart and drop an index bit. idxmap::tagIndexes is the
// only path from the tag to a subscript of @p tableIds; a 3-bit tag admits 8
// values and kCustomAttrType has 5 entries, so that pairing is checked here
// rather than trusted.
//
// It is a free function with external linkage, not a member and not static,
// because RowReader is a private nested type of MetadataTables: no test can
// name it, and every one of decodeRow's nineteen codedToken calls passes a
// literal tagBits of 1, 2, 3 or 5, so nothing that goes through them can reach
// the width that goes wrong. tests/cli_parser/cli_heaps_regression_test.cpp
// declares this signature and calls it directly.
MetadataToken splitCodedToken(uint32_t raw, const uint8_t* tableIds, size_t count, unsigned tagBits)
{
	MetadataToken tok;
	// 0xFF is what this decoder has always returned for a tag with no entry in
	// @p tableIds; index 0 is the null row, so tok.valid() is false. A refused
	// split leaves both, which is the one shape no caller can mistake for a
	// decoded token.
	tok.table = 0xFF;
	tok.index = 0;

	uint32_t tag = 0;
	uint32_t idx = 0;
	if (!utils::idxmap::splitTag(raw, tagBits, tag, idx)) return tok;

	tok.index = idx;
	if (tableIds != nullptr && utils::idxmap::tagIndexes(tag, count)) tok.table = tableIds[tag];
	return tok;
}

MetadataToken MetadataTables::RowReader::codedToken(const uint8_t* tableIds, size_t count, uint8_t tagBits)
{
	uint32_t raw = codedIdx(tableIds, count, tagBits);
	return splitCodedToken(raw, tableIds, count, tagBits);
}

// ─── Coded token tables ───────────────────────────────────────────────────────

static const uint8_t kTypeDefOrRef[] = {0x02, 0x01, 0x1B};             // 2 bits
static const uint8_t kHasConstant[] = {0x04, 0x08, 0x17};              // 2 bits
static const uint8_t kHasCustomAttr[] = {0x06, 0x04, 0x01, 0x02, 0x08, // 5 bits
										 0x09, 0x0A, 0x00, 0x11, 0x14, 0x17, 0x18, 0x1A, 0x1B,
										 0x20, 0x23, 0x26, 0x27, 0x28, 0x2A, 0x2B, 0x2C};
static const uint8_t kHasFieldMarshal[] = {0x04, 0x08};                   // 1 bit
static const uint8_t kHasDeclSecurity[] = {0x02, 0x06, 0x20};             // 2 bits
static const uint8_t kMemberRefParent[] = {0x02, 0x01, 0x1A, 0x06, 0x1B}; // 3 bits
static const uint8_t kHasSemantics[] = {0x14, 0x17};                      // 1 bit
static const uint8_t kMethodDefOrRef[] = {0x06, 0x0A};                    // 1 bit
static const uint8_t kMemberForwarded[] = {0x04, 0x06};                   // 1 bit
static const uint8_t kImplementation[] = {0x26, 0x23, 0x27};              // 2 bits
static const uint8_t kCustomAttrType[] = {0xFF, 0xFF, 0x06, 0x0A, 0xFF};  // 3 bits
static const uint8_t kResolutionScope[] = {0x00, 0x1A, 0x23, 0x01};       // 2 bits
static const uint8_t kTypeOrMethodDef[] = {0x02, 0x06};                   // 1 bit

// ─── MetadataTables::parse ────────────────────────────────────────────────────

bool MetadataTables::parse(std::span<const uint8_t> tilde, const CliHeaps& heaps)
{
	valid_ = false;

	// Every exit from here on must leave the object with no rows. This matters
	// on a reused MetadataTables: the row counts are copied for all 45 tables
	// before any table is parsed, so without this a stream that fails partway
	// leaves new counts standing over the previous file's buffers, and the
	// typed accessors read one against the other.
	auto dropRows = [this]() {
		for (auto& tbl: tables_)
		{
			tbl.rowCount = 0;
			tbl.rowSize = 0;
			tbl.data.clear();
		}
	};
	dropRows();

	if (tilde.size() < 24)
	{
		error_ = "#~ stream too small";
		return false;
	}

	// #~ stream header (§II.24.2.6)
	// DWORD  Reserved   (must be 0)
	// BYTE   MajorVersion
	// BYTE   MinorVersion
	// BYTE   HeapSizes
	// BYTE   Reserved2
	// QWORD  Valid        (bitmask of present tables)
	// QWORD  Sorted       (bitmask of sorted tables)
	// DWORD  Rows[popcount(Valid)]   (row counts for present tables)

	uint8_t heapSizes = tilde[6];
	uint64_t valid = 0;
	for (int i = 0; i < 8; ++i)
		valid |= static_cast<uint64_t>(tilde[8 + i]) << (i * 8);

	wideStrings_ = (heapSizes & kHeapSizeStrings) != 0;
	wideGuid_ = (heapSizes & kHeapSizeGUID) != 0;
	wideBlob_ = (heapSizes & kHeapSizeBlob) != 0;

	// Row counts
	size_t rowPos = 24;
	std::memset(rowCount_, 0, sizeof(rowCount_));
	for (int t = 0; t < 64 && t < static_cast<int>(TableId::_Count); ++t)
	{
		if (valid & (1ULL << t))
		{
			if (!utils::bounds::rangeFits(rowPos, tilde.size(), 4))
			{
				error_ = "#~ row count truncated";
				return false;
			}
			uint32_t rc = static_cast<uint32_t>(tilde[rowPos]) | (static_cast<uint32_t>(tilde[rowPos + 1]) << 8)
						| (static_cast<uint32_t>(tilde[rowPos + 2]) << 16)
						| (static_cast<uint32_t>(tilde[rowPos + 3]) << 24);
			rowCount_[t] = rc;
			rowPos += 4;
		}
	}

	// Copy to tables_ row counts
	for (int t = 0; t < static_cast<int>(TableId::_Count); ++t)
		tables_[t].rowCount = rowCount_[t];

	// Now parse each table's rows. The reader spans the whole #~ stream and
	// starts at the first row, so its bound is the bound the caller already
	// established on the stream; nothing downstream may read past it.
	RowReader rr;
	rr.data = tilde.data();
	rr.size = tilde.size();
	rr.pos = rowPos; // <= tilde.size(), checked in the loop above
	rr.wideStr = wideStrings_;
	rr.wideGuid = wideGuid_;
	rr.wideBlob = wideBlob_;
	rr.rowCounts = rowCount_;

	for (int t = 0; t < static_cast<int>(TableId::_Count); ++t)
	{
		if (rowCount_[t] == 0) continue;
		if (!parseTable(static_cast<TableId>(t), rr))
		{
			dropRows(); // a stream that failed to parse has no rows
			return false;
		}
	}

	valid_ = true;
	return true;
}

uint32_t MetadataTables::rowCount(TableId id) const
{
	size_t idx = static_cast<size_t>(id);
	if (idx >= kMaxTables) return 0;
	return tables_[idx].rowCount;
}

// ─── parseTable ──────────────────────────────────────────────────────────────

// The field layout of every decoded table, in one place. parseTable walks it to
// fill a row; computeRowSize walks it over a scratch buffer to learn how wide a
// row is. Keeping both on the same switch is the point: a second, hand-written
// copy of the widths is exactly the sort of thing that drifts out of step with
// the decoder and reintroduces the overrun this bound exists to stop.
void MetadataTables::decodeRow(TableId id, RowReader& rr, uint32_t* fields)
{
	size_t f = 0;

	// Field layout per table:
	switch (id)
	{
	case TableId::Module:
		fields[f++] = rr.u16();     // Generation
		fields[f++] = rr.strIdx();  // Name
		fields[f++] = rr.guidIdx(); // MvId
		fields[f++] = rr.guidIdx(); // EncId
		fields[f++] = rr.guidIdx(); // EncBaseId
		break;
	case TableId::TypeRef: {
		static const uint8_t tbl2[] = {0x00, 0x1A, 0x23, 0x01};
		auto tok = rr.codedToken(tbl2, 4, 2);
		fields[f++] = tok.table;
		fields[f++] = tok.index;   // ResolutionScope
		fields[f++] = rr.strIdx(); // Name
		fields[f++] = rr.strIdx(); // Namespace
		break;
	}
	case TableId::TypeDef: {
		fields[f++] = rr.u32();    // Flags
		fields[f++] = rr.strIdx(); // Name
		fields[f++] = rr.strIdx(); // Namespace
		auto tok = rr.codedToken(kTypeDefOrRef, 3, 2);
		fields[f++] = tok.table;
		fields[f++] = tok.index;                       // Extends
		fields[f++] = rr.tableIdx(TableId::Field);     // FieldList
		fields[f++] = rr.tableIdx(TableId::MethodDef); // MethodList
		break;
	}
	case TableId::Field:
		fields[f++] = rr.u16();     // Flags
		fields[f++] = rr.strIdx();  // Name
		fields[f++] = rr.blobIdx(); // Signature
		break;
	case TableId::MethodDef:
		fields[f++] = rr.u32();                    // RVA
		fields[f++] = rr.u16();                    // ImplFlags
		fields[f++] = rr.u16();                    // Flags
		fields[f++] = rr.strIdx();                 // Name
		fields[f++] = rr.blobIdx();                // Signature
		fields[f++] = rr.tableIdx(TableId::Param); // ParamList
		break;
	case TableId::Param:
		fields[f++] = rr.u16();    // Flags
		fields[f++] = rr.u16();    // Sequence
		fields[f++] = rr.strIdx(); // Name
		break;
	case TableId::InterfaceImpl:
		fields[f++] = rr.tableIdx(TableId::TypeDef); // Class
		{
			auto tok = rr.codedToken(kTypeDefOrRef, 3, 2);
			fields[f++] = tok.table;
			fields[f++] = tok.index;
		}
		break;
	case TableId::MemberRef: {
		auto tok = rr.codedToken(kMemberRefParent, 5, 3);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		fields[f++] = rr.strIdx();
		fields[f++] = rr.blobIdx();
		break;
	}
	case TableId::Constant: {
		fields[f++] = rr.u8();
		rr.u8(); // Type + padding
		auto tok = rr.codedToken(kHasConstant, 3, 2);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		fields[f++] = rr.blobIdx();
		break;
	}
	case TableId::CustomAttribute: {
		auto tok = rr.codedToken(kHasCustomAttr, static_cast<size_t>(22), 5);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		auto tok2 = rr.codedToken(kCustomAttrType, 5, 3);
		fields[f++] = tok2.table;
		fields[f++] = tok2.index;
		fields[f++] = rr.blobIdx();
		break;
	}
	case TableId::FieldMarshal: {
		auto tok = rr.codedToken(kHasFieldMarshal, 2, 1);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		fields[f++] = rr.blobIdx();
		break;
	}
	case TableId::ClassLayout:
		fields[f++] = rr.u16(); // PackingSize
		fields[f++] = rr.u32(); // ClassSize
		fields[f++] = rr.tableIdx(TableId::TypeDef);
		break;
	case TableId::StandAloneSig: fields[f++] = rr.blobIdx(); break;
	case TableId::PropertyMap:
		fields[f++] = rr.tableIdx(TableId::TypeDef);
		fields[f++] = rr.tableIdx(TableId::Property);
		break;
	case TableId::Property:
		fields[f++] = rr.u16();
		fields[f++] = rr.strIdx();
		fields[f++] = rr.blobIdx();
		break;
	case TableId::MethodSemantics: {
		fields[f++] = rr.u16();
		fields[f++] = rr.tableIdx(TableId::MethodDef);
		auto tok = rr.codedToken(kHasSemantics, 2, 1);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		break;
	}
	case TableId::MethodImpl: {
		fields[f++] = rr.tableIdx(TableId::TypeDef);
		auto body = rr.codedToken(kMethodDefOrRef, 2, 1);
		fields[f++] = body.table;
		fields[f++] = body.index;
		auto decl = rr.codedToken(kMethodDefOrRef, 2, 1);
		fields[f++] = decl.table;
		fields[f++] = decl.index;
		break;
	}
	case TableId::ModuleRef: fields[f++] = rr.strIdx(); break;
	case TableId::TypeSpec: fields[f++] = rr.blobIdx(); break;
	case TableId::ImplMap: {
		fields[f++] = rr.u16();
		auto tok = rr.codedToken(kMemberForwarded, 2, 1);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		fields[f++] = rr.strIdx();
		fields[f++] = rr.tableIdx(TableId::ModuleRef);
		break;
	}
	case TableId::FieldRVA:
		fields[f++] = rr.u32();
		fields[f++] = rr.tableIdx(TableId::Field);
		break;
	case TableId::Assembly:
		fields[f++] = rr.u32();     // HashAlgId
		fields[f++] = rr.u16();     // MajorVersion
		fields[f++] = rr.u16();     // MinorVersion
		fields[f++] = rr.u16();     // BuildNumber
		fields[f++] = rr.u16();     // RevisionNumber
		fields[f++] = rr.u32();     // Flags
		fields[f++] = rr.blobIdx(); // PublicKey
		fields[f++] = rr.strIdx();  // Name
		fields[f++] = rr.strIdx();  // Culture
		break;
	case TableId::AssemblyRef:
		fields[f++] = rr.u16();
		fields[f++] = rr.u16();
		fields[f++] = rr.u16();
		fields[f++] = rr.u16();
		fields[f++] = rr.u32();
		fields[f++] = rr.blobIdx();
		fields[f++] = rr.strIdx();
		fields[f++] = rr.strIdx();
		fields[f++] = rr.blobIdx();
		break;
	case TableId::NestedClass:
		fields[f++] = rr.tableIdx(TableId::TypeDef);
		fields[f++] = rr.tableIdx(TableId::TypeDef);
		break;
	case TableId::GenericParam: {
		fields[f++] = rr.u16(); // Number
		fields[f++] = rr.u16(); // Flags
		auto tok = rr.codedToken(kTypeOrMethodDef, 2, 1);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		fields[f++] = rr.strIdx();
		break;
	}
	case TableId::MethodSpec: {
		auto tok = rr.codedToken(kMethodDefOrRef, 2, 1);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		fields[f++] = rr.blobIdx();
		break;
	}
	case TableId::GenericParamConstraint: {
		fields[f++] = rr.tableIdx(TableId::GenericParam);
		auto tok = rr.codedToken(kTypeDefOrRef, 3, 2);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		break;
	}
	case TableId::EventMap:
		fields[f++] = rr.tableIdx(TableId::TypeDef);
		fields[f++] = rr.tableIdx(TableId::Event);
		break;
	case TableId::Event: {
		fields[f++] = rr.u16();
		fields[f++] = rr.strIdx();
		auto tok = rr.codedToken(kTypeDefOrRef, 3, 2);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		break;
	}
	case TableId::File:
		fields[f++] = rr.u32();
		fields[f++] = rr.strIdx();
		fields[f++] = rr.blobIdx();
		break;
	case TableId::ManifestResource: {
		fields[f++] = rr.u32();
		fields[f++] = rr.u32();
		fields[f++] = rr.strIdx();
		auto tok = rr.codedToken(kImplementation, 3, 2);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		break;
	}
	case TableId::ExportedType: {
		fields[f++] = rr.u32();
		fields[f++] = rr.u32();
		fields[f++] = rr.strIdx();
		fields[f++] = rr.strIdx();
		auto tok = rr.codedToken(kImplementation, 3, 2);
		fields[f++] = tok.table;
		fields[f++] = tok.index;
		break;
	}
	case TableId::DeclSecurity: {
		fields[f++] = rr.u16(); // Action
		auto tok = rr.codedToken(kHasDeclSecurity, 3, 2);
		fields[f++] = tok.table;
		fields[f++] = tok.index;    // Parent
		fields[f++] = rr.blobIdx(); // PermissionSet
		break;
	}
	case TableId::FieldLayout:
		fields[f++] = rr.u32();                    // Offset
		fields[f++] = rr.tableIdx(TableId::Field); // Field
		break;
	case TableId::AssemblyProcessor:
		fields[f++] = rr.u32(); // Processor
		break;
	case TableId::AssemblyOS:
		fields[f++] = rr.u32(); // OSPlatformID
		fields[f++] = rr.u32(); // OSMajorVersion
		fields[f++] = rr.u32(); // OSMinorVersion
		break;
	case TableId::AssemblyRefProcessor:
		fields[f++] = rr.u32();                          // Processor
		fields[f++] = rr.tableIdx(TableId::AssemblyRef); // AssemblyRef
		break;
	case TableId::AssemblyRefOS:
		fields[f++] = rr.u32();                          // OSPlatformID
		fields[f++] = rr.u32();                          // OSMajorVersion
		fields[f++] = rr.u32();                          // OSMinorVersion
		fields[f++] = rr.tableIdx(TableId::AssemblyRef); // AssemblyRef
		break;
	default:
		// A table this decoder has no layout for. Rows are laid out end to end
		// with no length prefix and no padding, so a table whose width is
		// unknown is not merely unreadable itself -- it hides where the next
		// table starts, and every table after it decodes at the wrong offset.
		//
		// This arm used to fall through silently, on the stated grounds that
		// such tables "are always empty in practice". They are not: this is
		// where DeclSecurity and FieldLayout landed, and both appear in
		// ordinary assemblies (any signed assembly, any explicit-layout
		// struct). Those six are decoded above now. What is left is the
		// uncompressed-metadata and edit-and-continue tables, which the `#~`
		// stream this reader accepts does not carry, so reaching here means
		// the stream is not what it claims to be.
		//
		// Marking the reader truncated is what says so: computeRowSize returns
		// 0 for such a table and parseTable turns that into a parse failure,
		// rather than reporting rows it has no way to locate.
		rr.truncated = true;
		break;
	}
	(void)f;
}

// ─── computeRowSize ────────────────────────────────────────────────

size_t MetadataTables::computeRowSize(TableId id) const
{
	// Row width depends only on the heap-size flags and the row counts of the
	// referenced tables ─ never on the row bytes themselves. So the honest way
	// to get it is to run the decoder over a zeroed scratch row and measure how
	// far it advanced, rather than maintaining a second table of widths.
	//
	// Returns 0 for a table this decoder has no layout for -- decodeRow marks
	// the probe truncated in that case. parseTable turns a zero width into a
	// parse failure rather than a row count it cannot bound: such a table
	// consumes no bytes, so countFits would hand it the entire remaining
	// stream as its row count and parseTable would zero-fill 48 bytes per
	// "row" -- roughly 48x the stream per undecoded table, and the six that
	// used to land here compounded to nearly 300x. The row data would be
	// meaningless either way, since not advancing the cursor desynchronises
	// every table after it.
	uint8_t scratch[kMaxRowBytes] = {};

	RowReader probe;
	probe.data = scratch;
	probe.size = sizeof(scratch);
	probe.pos = 0;
	probe.wideStr = wideStrings_;
	probe.wideGuid = wideGuid_;
	probe.wideBlob = wideBlob_;
	probe.rowCounts = rowCount_;

	uint32_t fields[kMaxFields] = {};
	decodeRow(id, probe, fields);

	// kMaxRowBytes is meant to cover every layout; if it somehow did not, the
	// measurement is not trustworthy and must not be used as a bound.
	return probe.truncated ? 0 : probe.pos;
}

// ─── parseTable ───────────────────────────────────────────────────

bool MetadataTables::parseTable(TableId id, RowReader& rr)
{
	auto& tbl = tables_[static_cast<size_t>(id)];
	uint32_t n = tbl.rowCount;
	if (n == 0) return true;

	// The row count came out of the stream header, and nothing had checked it
	// against the stream. Bound it against the bytes still ahead of this table
	// at this table's own row width: a count the remaining input cannot supply
	// is malformed by construction, and used to both run the reader off the end
	// of the buffer and size the row buffer below at hundreds of gigabytes.
	const size_t rowBytes = computeRowSize(id);
	if (rowBytes == 0)
	{
		// See computeRowSize: no layout, so no way to say where this table
		// ends or the next one begins, and no bound to hold the row count to.
		error_ = "#~ stream declares a table this reader cannot lay out";
		return false;
	}
	if (!utils::bounds::countFits(rr.pos, rr.size, n, rowBytes))
	{
		error_ = "#~ table declares more rows than the stream can hold";
		return false;
	}

	// We store rows in a generic byte buffer, then access them via typed methods.
	// For simplicity, store each row as a vector of up to kMaxFields uint32_t
	// fields. This avoids per-table struct sizing complexity in the parse loop.
	// Each row is stored as: [field0, field1, …, fieldN-1] as uint32_t values.
	tbl.rowSize = kMaxFields * sizeof(uint32_t);

	// In size_t, not uint32_t: `n * tbl.rowSize` was unsigned-int arithmetic, so
	// 0x10000000 rows of 48 bytes (exactly 3·2^32) wrapped to zero and the loop
	// below then wrote rows through a zero-length buffer. The product is not
	// formed until mulFits says it is representable, for the same reason.
	if (!utils::bounds::mulFits(static_cast<size_t>(n), tbl.rowSize))
	{
		error_ = "#~ table row storage overflows";
		return false;
	}
	tbl.data.assign(static_cast<size_t>(n) * tbl.rowSize, 0);

	for (uint32_t row = 0; row < n; ++row)
	{
		uint32_t* fields = reinterpret_cast<uint32_t*>(tbl.data.data() + static_cast<size_t>(row) * tbl.rowSize);
		decodeRow(id, rr, fields);
		if (rr.truncated)
		{
			// Not reachable through a well-formed path: countFits above was
			// evaluated at this table's own measured row width, so the bytes
			// every row needs are guaranteed to be there. Reaching here means
			// computeRowSize and decodeRow disagreed about the width -- the
			// one thing sharing this switch between them is meant to prevent.
			// Fail rather than hand back rows read from somewhere unknown.
			error_ = "#~ table row truncated";
			return false;
		}
	}
	return true;
}

// ─── Typed row accessors ──────────────────────────────────────────────────────

static const uint32_t* rowFields(const RawTable& tbl, uint32_t idx)
{
	if (idx == 0 || idx > tbl.rowCount) return nullptr;
	// rowCount alone is not enough to say the row is there. It is set for every
	// table in the Valid mask before any of them is parsed, so it can outrun
	// what was actually allocated; and rowSize is 0 on a table that was never
	// parsed at all. Ask the buffer, which is the thing being indexed: rowSize
	// rows of `rowSize` bytes have to be inside it. parseTable sets rowSize to
	// the full row width whenever it allocates, so this both rejects the
	// never-parsed case and bounds the read for the parsed one.
	const size_t rowSize = tbl.rowSize;
	if (rowSize == 0) return nullptr;
	if (!utils::bounds::countFits(0, tbl.data.size(), idx, rowSize)) return nullptr;
	return reinterpret_cast<const uint32_t*>(tbl.data.data() + static_cast<size_t>(idx - 1) * rowSize);
}

ModuleRow MetadataTables::module(uint32_t idx) const
{
	const auto* f = rowFields(tables_[0], idx);
	if (!f) return {};
	return {static_cast<uint16_t>(f[0]), f[1], f[2], f[3], f[4]};
}

TypeRefRow MetadataTables::typeRef(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::TypeRef)], idx);
	if (!f) return {};
	TypeRefRow r;
	r.resolutionScope = {static_cast<uint8_t>(f[0]), f[1]};
	r.name = f[2];
	r.ns = f[3];
	return r;
}

TypeDefRow MetadataTables::typeDef(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::TypeDef)], idx);
	if (!f) return {};
	TypeDefRow r;
	r.flags = f[0];
	r.name = f[1];
	r.ns = f[2];
	r.extends = {static_cast<uint8_t>(f[3]), f[4]};
	r.fieldList = f[5];
	r.methodList = f[6];
	return r;
}

FieldRow MetadataTables::field(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Field)], idx);
	if (!f) return {};
	return {static_cast<uint16_t>(f[0]), f[1], f[2]};
}

MethodDefRow MetadataTables::methodDef(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::MethodDef)], idx);
	if (!f) return {};
	return {f[0], static_cast<uint16_t>(f[1]), static_cast<uint16_t>(f[2]), f[3], f[4], f[5]};
}

ParamRow MetadataTables::param(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Param)], idx);
	if (!f) return {};
	return {static_cast<uint16_t>(f[0]), static_cast<uint16_t>(f[1]), f[2]};
}

InterfaceImplRow MetadataTables::interfaceImpl(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::InterfaceImpl)], idx);
	if (!f) return {};
	InterfaceImplRow r;
	r.clazz = f[0];
	r.interface_ = {static_cast<uint8_t>(f[1]), f[2]};
	return r;
}

MemberRefRow MetadataTables::memberRef(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::MemberRef)], idx);
	if (!f) return {};
	MemberRefRow r;
	r.clazz = {static_cast<uint8_t>(f[0]), f[1]};
	r.name = f[2];
	r.signature = f[3];
	return r;
}

ConstantRow MetadataTables::constant(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Constant)], idx);
	if (!f) return {};
	ConstantRow r;
	r.type = static_cast<uint8_t>(f[0]);
	r.parent = {static_cast<uint8_t>(f[1]), f[2]};
	r.value = f[3];
	return r;
}

CustomAttributeRow MetadataTables::customAttribute(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::CustomAttribute)], idx);
	if (!f) return {};
	CustomAttributeRow r;
	r.parent = {static_cast<uint8_t>(f[0]), f[1]};
	r.type = {static_cast<uint8_t>(f[2]), f[3]};
	r.value = f[4];
	return r;
}

FieldMarshalRow MetadataTables::fieldMarshal(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::FieldMarshal)], idx);
	if (!f) return {};
	FieldMarshalRow r;
	r.parent = {static_cast<uint8_t>(f[0]), f[1]};
	r.nativeType = f[2];
	return r;
}

ClassLayoutRow MetadataTables::classLayout(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::ClassLayout)], idx);
	if (!f) return {};
	return {static_cast<uint16_t>(f[0]), f[1], f[2]};
}

StandAloneSigRow MetadataTables::standAloneSig(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::StandAloneSig)], idx);
	if (!f) return {};
	return {f[0]};
}

PropertyRow MetadataTables::property(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Property)], idx);
	if (!f) return {};
	return {static_cast<uint16_t>(f[0]), f[1], f[2]};
}

MethodSemanticsRow MetadataTables::methodSemantics(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::MethodSemantics)], idx);
	if (!f) return {};
	MethodSemanticsRow r;
	r.semantics = static_cast<uint16_t>(f[0]);
	r.method = f[1];
	r.association = {static_cast<uint8_t>(f[2]), f[3]};
	return r;
}

MethodImplRow MetadataTables::methodImpl(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::MethodImpl)], idx);
	if (!f) return {};
	MethodImplRow r;
	r.clazz = f[0];
	r.methodBody = {static_cast<uint8_t>(f[1]), f[2]};
	r.methodDeclaration = {static_cast<uint8_t>(f[3]), f[4]};
	return r;
}

ModuleRefRow MetadataTables::moduleRef(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::ModuleRef)], idx);
	if (!f) return {};
	return {f[0]};
}

TypeSpecRow MetadataTables::typeSpec(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::TypeSpec)], idx);
	if (!f) return {};
	return {f[0]};
}

ImplMapRow MetadataTables::implMap(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::ImplMap)], idx);
	if (!f) return {};
	ImplMapRow r;
	r.mappingFlags = static_cast<uint16_t>(f[0]);
	r.memberForwarded = {static_cast<uint8_t>(f[1]), f[2]};
	r.importName = f[3];
	r.importScope = f[4];
	return r;
}

FieldRVARow MetadataTables::fieldRVA(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::FieldRVA)], idx);
	if (!f) return {};
	return {f[0], f[1]};
}

AssemblyRow MetadataTables::assembly(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Assembly)], idx);
	if (!f) return {};
	AssemblyRow r;
	r.hashAlgId = f[0];
	r.majorVersion = static_cast<uint16_t>(f[1]);
	r.minorVersion = static_cast<uint16_t>(f[2]);
	r.buildNumber = static_cast<uint16_t>(f[3]);
	r.revisionNumber = static_cast<uint16_t>(f[4]);
	r.flags = f[5];
	r.publicKey = f[6];
	r.name = f[7];
	r.culture = f[8];
	return r;
}

AssemblyRefRow MetadataTables::assemblyRef(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::AssemblyRef)], idx);
	if (!f) return {};
	AssemblyRefRow r;
	r.majorVersion = static_cast<uint16_t>(f[0]);
	r.minorVersion = static_cast<uint16_t>(f[1]);
	r.buildNumber = static_cast<uint16_t>(f[2]);
	r.revisionNumber = static_cast<uint16_t>(f[3]);
	r.flags = f[4];
	r.publicKeyOrToken = f[5];
	r.name = f[6];
	r.culture = f[7];
	r.hashValue = f[8];
	return r;
}

NestedClassRow MetadataTables::nestedClass(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::NestedClass)], idx);
	if (!f) return {};
	return {f[0], f[1]};
}

GenericParamRow MetadataTables::genericParam(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::GenericParam)], idx);
	if (!f) return {};
	GenericParamRow r;
	r.number = static_cast<uint16_t>(f[0]);
	r.flags = static_cast<uint16_t>(f[1]);
	r.owner = {static_cast<uint8_t>(f[2]), f[3]};
	r.name = f[4];
	return r;
}

MethodSpecRow MetadataTables::methodSpec(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::MethodSpec)], idx);
	if (!f) return {};
	MethodSpecRow r;
	r.method = {static_cast<uint8_t>(f[0]), f[1]};
	r.instantiation = f[2];
	return r;
}

GenericParamConstraintRow MetadataTables::genericParamConstraint(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::GenericParamConstraint)], idx);
	if (!f) return {};
	GenericParamConstraintRow r;
	r.owner = f[0];
	r.constraint = {static_cast<uint8_t>(f[1]), f[2]};
	return r;
}

EventRow MetadataTables::event(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::Event)], idx);
	if (!f) return {};
	EventRow r;
	r.flags = static_cast<uint16_t>(f[0]);
	r.name = f[1];
	r.eventType = {static_cast<uint8_t>(f[2]), f[3]};
	return r;
}

PropertyMapRow MetadataTables::propertyMap(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::PropertyMap)], idx);
	if (!f) return {};
	return {f[0], f[1]};
}

EventMapRow MetadataTables::eventMap(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::EventMap)], idx);
	if (!f) return {};
	return {f[0], f[1]};
}

FileRow MetadataTables::file(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::File)], idx);
	if (!f) return {};
	return {f[0], f[1], f[2]};
}

ManifestResourceRow MetadataTables::manifestResource(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::ManifestResource)], idx);
	if (!f) return {};
	ManifestResourceRow r;
	r.offset = f[0];
	r.flags = f[1];
	r.name = f[2];
	r.implementation = {static_cast<uint8_t>(f[3]), f[4]};
	return r;
}

ExportedTypeRow MetadataTables::exportedType(uint32_t idx) const
{
	const auto* f = rowFields(tables_[static_cast<size_t>(TableId::ExportedType)], idx);
	if (!f) return {};
	ExportedTypeRow r;
	r.flags = f[0];
	r.typeDefId = f[1];
	r.typeName = f[2];
	r.typeNamespace = f[3];
	r.implementation = {static_cast<uint8_t>(f[4]), f[5]};
	return r;
}

// ─── Coded token decoders ─────────────────────────────────────────────────────

MetadataToken MetadataTables::decodeTypeDefOrRef(uint32_t coded) const
{
	uint32_t tag = coded & 0x3;
	uint32_t idx = coded >> 2;
	if (tag >= 3) return {};
	return {kTypeDefOrRef[tag], idx};
}

MetadataToken MetadataTables::decodeResolutionScope(uint32_t coded) const
{
	uint32_t tag = coded & 0x3;
	uint32_t idx = coded >> 2;
	if (tag >= 4) return {};
	return {kResolutionScope[tag], idx};
}

MetadataToken MetadataTables::decodeMemberRefParent(uint32_t coded) const
{
	uint32_t tag = coded & 0x7;
	uint32_t idx = coded >> 3;
	if (tag >= 5) return {};
	return {kMemberRefParent[tag], idx};
}

MetadataToken MetadataTables::decodeHasCustomAttribute(uint32_t coded) const
{
	uint32_t tag = coded & 0x1F;
	uint32_t idx = coded >> 5;
	if (tag >= 22) return {};
	return {kHasCustomAttr[tag], idx};
}

MetadataToken MetadataTables::decodeCustomAttributeType(uint32_t coded) const
{
	uint32_t tag = coded & 0x7;
	uint32_t idx = coded >> 3;
	if (tag >= 5) return {};
	return {kCustomAttrType[tag], idx};
}

MetadataToken MetadataTables::decodeTypeOrMethodDef(uint32_t coded) const
{
	uint32_t tag = coded & 0x1;
	uint32_t idx = coded >> 1;
	return {kTypeOrMethodDef[tag], idx};
}

MetadataToken MetadataTables::decodeMethodDefOrRef(uint32_t coded) const
{
	uint32_t tag = coded & 0x1;
	uint32_t idx = coded >> 1;
	return {kMethodDefOrRef[tag], idx};
}

MetadataToken MetadataTables::decodeHasSemantics(uint32_t coded) const
{
	uint32_t tag = coded & 0x1;
	uint32_t idx = coded >> 1;
	return {kHasSemantics[tag], idx};
}

MetadataToken MetadataTables::decodeMemberForwarded(uint32_t coded) const
{
	uint32_t tag = coded & 0x1;
	uint32_t idx = coded >> 1;
	return {kMemberForwarded[tag], idx};
}

MetadataToken MetadataTables::decodeImplementation(uint32_t coded) const
{
	uint32_t tag = coded & 0x3;
	uint32_t idx = coded >> 2;
	if (tag >= 3) return {};
	return {kImplementation[tag], idx};
}

MetadataToken MetadataTables::decodeHasConstant(uint32_t coded) const
{
	uint32_t tag = coded & 0x3;
	uint32_t idx = coded >> 2;
	if (tag >= 3) return {};
	return {kHasConstant[tag], idx};
}

} // namespace cli_parser
} // namespace retdec
