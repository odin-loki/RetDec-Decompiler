/**
 * @file tests/debug_info/pdb_builder.h
 * @brief Minimal in-memory MSF / PDB7 builders shared by the test suites.
 *
 * Every test in this tree builds its inputs in memory rather than reading a
 * checked-in binary, so nothing depends on a working directory. These three
 * builders produce a PDB7 file that PDBFile::load_pdb_file accepts, with
 * CodeView symbol and TPI type records of the caller's choosing.
 *
 * They target PdbExtractor, which parses the MSF itself; they are not built
 * to satisfy pdbparser's PDBFile::load_pdb_file, which checks the container
 * more strictly (tests/pdbparser builds its own minimal file for that).
 *
 * They started inline in debug_info_test.cpp, which is 1,500 lines without
 * them. Header-only and self-contained; include it with a relative path.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_TESTS_DEBUG_INFO_PDB_BUILDER_H
#define RETDEC_TESTS_DEBUG_INFO_PDB_BUILDER_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace retdec {
namespace tests {
namespace pdb_builder {

// ── Minimal MSF PDB7 builder ─────────────────────────────────────────────────

static constexpr char kMsfMagic[] = "Microsoft C/C++ MSF 7.00\r\n\x1a\x44\x53\x00\x00\x00";

class PdbBuilder {
public:
	static constexpr uint32_t kBlockSize = 512;

	// Build a minimal PDB7 MSF file with:
	//   Stream 0: PDB info stream (minimal)
	//   Stream 1: TPI stream (with optional LF_STRUCTURE record)
	//   Stream 2: DBI stream containing a fake symbol stream
	//   Stream 3: IPI stream (empty)
	//   Stream 4: GSI stream (public symbols)
	//
	// symbolStream: raw bytes of a CodeView symbol stream (may include S_GPROC32 etc)
	// typeStream:   raw bytes of a TPI type stream

	static std::vector<uint8_t> build(const std::vector<uint8_t>& symbolStream, const std::vector<uint8_t>& typeStream)
	{
		// Streams: [pdb_info, tpi, dbi, ipi, gsi]
		std::vector<std::vector<uint8_t>> streams;
		streams.push_back(makePdbInfoStream()); // 0
		streams.push_back(typeStream);          // 1 TPI
		streams.push_back(symbolStream);        // 2 DBI (repurposed as sym stream)
		streams.push_back({});                  // 3 IPI
		streams.push_back({});                  // 4 GSI

		// Assign blocks for each stream
		std::vector<std::vector<uint32_t>> streamBlocks;
		uint32_t nextBlock = 3; // 0=superblock, 1=FPM, 2=FPM2
		for (const auto& s: streams)
		{
			uint32_t nb = s.empty() ? 0 : blocksNeeded(s.size(), kBlockSize);
			std::vector<uint32_t> bl;
			for (uint32_t i = 0; i < nb; ++i)
				bl.push_back(nextBlock++);
			streamBlocks.push_back(std::move(bl));
		}

		// Build directory
		std::vector<uint8_t> dir;
		uint32_t numStreams = static_cast<uint32_t>(streams.size());
		append32(dir, numStreams);
		for (const auto& s: streams)
			append32(dir, s.empty() ? 0 : static_cast<uint32_t>(s.size()));
		for (const auto& bl: streamBlocks)
			for (uint32_t b: bl)
				append32(dir, b);

		// Assign blocks for directory itself
		std::vector<uint32_t> dirBlocks;
		uint32_t dirNb = blocksNeeded(dir.size(), kBlockSize);
		for (uint32_t i = 0; i < dirNb; ++i)
			dirBlocks.push_back(nextBlock++);

		// Total blocks
		uint32_t totalBlocks = nextBlock;

		// Allocate file storage
		std::vector<uint8_t> file(uint64_t(totalBlocks) * kBlockSize, 0);

		// Write superblock (block 0)
		{
			uint8_t* sb = file.data();
			std::memcpy(sb, kMsfMagic, 32);
			write32(sb + 32, kBlockSize);
			write32(sb + 36, 1); // free block map block
			write32(sb + 40, totalBlocks);
			write32(sb + 44, static_cast<uint32_t>(dir.size()));
			write32(sb + 48, 0);            // unknown
			write32(sb + 52, dirBlocks[0]); // blockMapAddr = first dir block index list block
		}

		// Write directory block list at blockMapAddr (dirBlocks[0])
		{
			uint8_t* bm = file.data() + uint64_t(dirBlocks[0]) * kBlockSize;
			for (uint32_t i = 0; i < dirNb; ++i)
				write32(bm + 4 * i, dirBlocks[i]);
		}

		// Write directory stream
		for (uint32_t i = 0; i < dirNb; ++i)
		{
			std::size_t off = uint64_t(dirBlocks[i]) * kBlockSize;
			std::size_t src = uint64_t(i) * kBlockSize;
			std::size_t take = std::min<std::size_t>(kBlockSize, dir.size() - src);
			std::memcpy(file.data() + off, dir.data() + src, take);
		}

		// Write each stream
		for (uint32_t si = 0; si < numStreams; ++si)
		{
			const auto& s = streams[si];
			const auto& bl = streamBlocks[si];
			for (uint32_t bi = 0; bi < bl.size(); ++bi)
			{
				std::size_t off = uint64_t(bl[bi]) * kBlockSize;
				std::size_t src = uint64_t(bi) * kBlockSize;
				std::size_t take = std::min<std::size_t>(kBlockSize, s.size() - src);
				std::memcpy(file.data() + off, s.data() + src, take);
			}
		}

		return file;
	}

private:
	static uint32_t blocksNeeded(std::size_t bytes, uint32_t bs)
	{
		return static_cast<uint32_t>((bytes + bs - 1) / bs);
	}
	static void append32(std::vector<uint8_t>& v, uint32_t x)
	{
		v.push_back(x & 0xFF);
		v.push_back((x >> 8) & 0xFF);
		v.push_back((x >> 16) & 0xFF);
		v.push_back((x >> 24) & 0xFF);
	}
	static void write32(uint8_t* p, uint32_t x)
	{
		p[0] = x;
		p[1] = x >> 8;
		p[2] = x >> 16;
		p[3] = x >> 24;
	}
	static std::vector<uint8_t> makePdbInfoStream()
	{
		std::vector<uint8_t> v;
		append32(v, 20191201); // PDB version (VC140)
		append32(v, 0);        // timestamp
		append32(v, 1);        // age
		// GUID (16 bytes)
		for (int i = 0; i < 16; ++i)
			v.push_back(static_cast<uint8_t>(i));
		return v;
	}
};

// ── CodeView symbol stream builder ───────────────────────────────────────────

class CvSymBuilder {
public:
	static constexpr uint16_t S_GPROC32 = 0x1110;
	static constexpr uint16_t S_LPROC32 = 0x110f;

	CvSymBuilder()
	{
		// CV_SIGNATURE = 4
		append32(data_, 4);
	}

	void addGProc32(const std::string& name, uint32_t off, uint32_t len, uint32_t typeIdx = 0x0075)
	{
		addProc(S_GPROC32, name, off, len, typeIdx);
	}
	void addLProc32(const std::string& name, uint32_t off, uint32_t len, uint32_t typeIdx = 0x0075)
	{
		addProc(S_LPROC32, name, off, len, typeIdx);
	}

	std::vector<uint8_t> data() const
	{
		return data_;
	}

private:
	std::vector<uint8_t> data_;

	static void append32(std::vector<uint8_t>& v, uint32_t x)
	{
		v.push_back(x);
		v.push_back(x >> 8);
		v.push_back(x >> 16);
		v.push_back(x >> 24);
	}
	static void append16(std::vector<uint8_t>& v, uint16_t x)
	{
		v.push_back(x);
		v.push_back(x >> 8);
	}

	void addProc(uint16_t kind, const std::string& name, uint32_t off, uint32_t len, uint32_t typeIdx)
	{
		// ProcSym32: pParent, pEnd, pNext, len, dbgStart, dbgEnd, typind, off, seg, flags
		// = 4+4+4+4+4+4+4+4+2+1 = 39 bytes + name + null
		std::vector<uint8_t> rec;
		append16(rec, kind);
		append32(rec, 0); // pParent
		append32(rec, 0); // pEnd
		append32(rec, 0); // pNext
		append32(rec, len);
		append32(rec, 0);                     // dbgStart
		append32(rec, len > 0 ? len - 1 : 0); // dbgEnd
		append32(rec, typeIdx);
		append32(rec, off);
		append16(rec, 1); // seg
		rec.push_back(0); // flags
		for (char c: name)
			rec.push_back(static_cast<uint8_t>(c));
		rec.push_back(0); // null terminator
		// Align to 4
		while (rec.size() % 4)
			rec.push_back(0xf4);

		// Length field = rec.size() - 2 (excludes the 2-byte length field itself)
		uint16_t recLen = static_cast<uint16_t>(rec.size() - 2);
		data_.push_back(recLen & 0xFF);
		data_.push_back(recLen >> 8);
		data_.insert(data_.end(), rec.begin() + 2, rec.end());
	}
};

// ── TPI stream builder ────────────────────────────────────────────────────────

class TpiBuilder {
public:
	// TPI header: 56 bytes
	// typeIndexBegin = 0x1000
	// typeIndexEnd   = 0x1000 + count
	TpiBuilder(): typeIdx_(0x1000) {}

	// Add an LF_STRUCTURE record
	uint32_t addStructure(const std::string& name, uint16_t byteSize)
	{
		std::vector<uint8_t> rec;
		append16(rec, 0x1505); // LF_STRUCTURE
		append16(rec, 0);      // count
		append16(rec, 0);      // prop
		append32(rec, 0);      // field list
		append32(rec, 0);      // derived
		append32(rec, 0);      // vshape
		// Numeric leaf (byte_size): value < 0x8000 → raw short
		append16(rec, byteSize);
		for (char c: name)
			rec.push_back(static_cast<uint8_t>(c));
		rec.push_back(0);
		while (rec.size() % 4)
			rec.push_back(0);
		addRec(rec);
		return typeIdx_++;
	}

	// Add an LF_ENUM record: count(2) property(2) utype(4) field(4), name.
	uint32_t addEnum(const std::string& name, uint32_t underlying)
	{
		std::vector<uint8_t> rec;
		append16(rec, 0x1507); // LF_ENUM
		append16(rec, 0);      // count
		append16(rec, 0);      // property
		append32(rec, underlying);
		append32(rec, 0); // field list
		for (char c: name)
			rec.push_back(static_cast<uint8_t>(c));
		rec.push_back(0);
		while (rec.size() % 4)
			rec.push_back(0);
		addRec(rec);
		return typeIdx_++;
	}

	// Add an LF_UNION record: count(2) property(2) field(4), numeric size, name.
	uint32_t addUnion(const std::string& name, uint16_t byteSize)
	{
		std::vector<uint8_t> rec;
		append16(rec, 0x1506); // LF_UNION
		append16(rec, 0);      // count
		append16(rec, 0);      // property
		append32(rec, 0);      // field list
		append16(rec, byteSize);
		for (char c: name)
			rec.push_back(static_cast<uint8_t>(c));
		rec.push_back(0);
		while (rec.size() % 4)
			rec.push_back(0);
		addRec(rec);
		return typeIdx_++;
	}

	// Add an LF_ARRAY record: elemtype(4) idxtype(4), numeric size, name.
	uint32_t addArray(uint32_t elemType, uint16_t byteSize)
	{
		std::vector<uint8_t> rec;
		append16(rec, 0x1503); // LF_ARRAY
		append32(rec, elemType);
		append32(rec, 0x23); // index type: T_UQUAD
		append16(rec, byteSize);
		rec.push_back(0); // empty name
		while (rec.size() % 4)
			rec.push_back(0);
		addRec(rec);
		return typeIdx_++;
	}

	// Add an LF_POINTER record
	uint32_t addPointer(uint32_t pointedTo, bool is64bit = true)
	{
		std::vector<uint8_t> rec;
		append16(rec, 0x1002); // LF_POINTER
		append32(rec, pointedTo);
		uint32_t ptrAttr = is64bit ? (8 | 0x0C00) : 4; // size in attr
		if (is64bit) ptrAttr = (0x0C << 8) | 8;        // 64-bit flat
		append32(rec, ptrAttr);
		addRec(rec);
		return typeIdx_++;
	}

	std::vector<uint8_t> build() const
	{
		std::vector<uint8_t> hdr(56, 0);
		// version = 20040203
		write32(hdr.data(), 20040203u);
		write32(hdr.data() + 4, 56);        // headerSize
		write32(hdr.data() + 8, 0x1000);    // typeIndexBegin
		write32(hdr.data() + 12, typeIdx_); // typeIndexEnd
		uint32_t recBytes = static_cast<uint32_t>(typeRecs_.size());
		write32(hdr.data() + 16, recBytes);
		// rest zeroed (hash stream index etc)
		hdr.insert(hdr.end(), typeRecs_.begin(), typeRecs_.end());
		return hdr;
	}

private:
	uint32_t typeIdx_;
	std::vector<uint8_t> typeRecs_;

	void addRec(const std::vector<uint8_t>& rec)
	{
		// Prepend 2-byte length (little-endian) = rec.size()
		uint16_t len = static_cast<uint16_t>(rec.size());
		typeRecs_.push_back(len & 0xFF);
		typeRecs_.push_back(len >> 8);
		typeRecs_.insert(typeRecs_.end(), rec.begin(), rec.end());
	}

	static void append16(std::vector<uint8_t>& v, uint16_t x)
	{
		v.push_back(x);
		v.push_back(x >> 8);
	}
	static void append32(std::vector<uint8_t>& v, uint32_t x)
	{
		v.push_back(x);
		v.push_back(x >> 8);
		v.push_back(x >> 16);
		v.push_back(x >> 24);
	}
	static void write32(uint8_t* p, uint32_t x)
	{
		p[0] = x;
		p[1] = x >> 8;
		p[2] = x >> 16;
		p[3] = x >> 24;
	}
};

} // namespace pdb_builder
} // namespace tests
} // namespace retdec

#endif // RETDEC_TESTS_DEBUG_INFO_PDB_BUILDER_H
