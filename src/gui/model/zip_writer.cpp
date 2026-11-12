/**
 * @file src/gui/model/zip_writer.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/zip_writer.h"

#include <forge/os/os.hpp>

#include <array>
#include <format>

namespace retdec::gui::model {

namespace {

constexpr uint32_t kLocalFileHeaderSig = 0x04034b50;
constexpr uint32_t kCentralDirHeaderSig = 0x02014b50;
constexpr uint32_t kEndOfCentralDirSig = 0x06054b50;
// 1980-01-01 00:00, the earliest DOS date: a valid timestamp that keeps the
// archive byte-for-byte reproducible.
constexpr uint16_t kDosTime = 0;
constexpr uint16_t kDosDate = (0 << 9) | (1 << 5) | 1;
constexpr uint16_t kFlagUtf8Names = 1u << 11;

void appendLe16(std::string& out, uint16_t v)
{
	out.push_back(static_cast<char>(v & 0xff));
	out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void appendLe32(std::string& out, uint32_t v)
{
	out.push_back(static_cast<char>(v & 0xff));
	out.push_back(static_cast<char>((v >> 8) & 0xff));
	out.push_back(static_cast<char>((v >> 16) & 0xff));
	out.push_back(static_cast<char>((v >> 24) & 0xff));
}

uint16_t nameFlags(std::string_view name)
{
	for (unsigned char c: name)
		if (c >= 0x80) return kFlagUtf8Names;
	return 0;
}

const std::array<uint32_t, 256>& crcTable()
{
	static const std::array<uint32_t, 256> table = [] {
		std::array<uint32_t, 256> t{};
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
			t[i] = c;
		}
		return t;
	}();
	return table;
}

} // namespace

struct ZipWriter::Impl
{
	forge::os::file file;
};

ZipWriter::ZipWriter(const std::filesystem::path& zipPath): path_(zipPath)
{
	auto f = forge::os::file::open(path_.string(), forge::os::wr | forge::os::create | forge::os::truncate);
	if (!f)
	{
		error_ = std::format("Cannot create {}: {}", path_.string(), f.err.what);
		return;
	}
	impl_ = std::make_unique<Impl>();
	impl_->file = std::move(f.value);
	open_ = true;
}

ZipWriter::~ZipWriter()
{
	if (open_ && !closed_) (void) close();
}

uint32_t ZipWriter::crc32(std::string_view data)
{
	const auto& table = crcTable();
	uint32_t crc = 0xffffffffu;
	for (unsigned char ch: data)
		crc = table[(crc ^ ch) & 0xffu] ^ (crc >> 8);
	return crc ^ 0xffffffffu;
}

std::vector<std::string> ZipWriter::entryNames() const
{
	std::vector<std::string> out;
	out.reserve(entries_.size());
	for (const Entry& e: entries_)
		out.push_back(e.name);
	return out;
}

std::expected<void, std::string> ZipWriter::fail(std::string msg)
{
	error_ = std::move(msg);
	return std::unexpected(error_);
}

std::expected<void, std::string> ZipWriter::writeBytes(std::string_view bytes)
{
	if (bytes.empty()) return {};
	if (auto st = impl_->file.write_all(bytes.data(), bytes.size()); !st)
		return fail(std::format("Cannot write {}: {}", path_.string(), st.err.what));
	offset_ += bytes.size();
	return {};
}

std::expected<void, std::string> ZipWriter::addEntry(std::string_view entryName, std::string_view data)
{
	if (!open_ || closed_) return fail("ZIP archive is not open");

	std::string name(entryName);
	for (char& c: name)
		if (c == '\\') c = '/';
	if (name.empty()) return fail("Empty ZIP entry name");
	if (name.front() == '/') return fail(std::format("Invalid ZIP entry name: {}", entryName));
	if (name.size() > 0xffff) return fail(std::format("ZIP entry name too long: {}", entryName));
	if (entries_.size() >= 0xffff) return fail("Too many ZIP entries (no ZIP64 support)");
	if (data.size() > 0xffffffffull || offset_ + data.size() + 30 + name.size() > 0xffffffffull)
		return fail("ZIP archive too large (no ZIP64 support)");

	const uint32_t crc = crc32(data);
	const uint32_t offset = static_cast<uint32_t>(offset_);
	const uint32_t size = static_cast<uint32_t>(data.size());

	std::string hdr;
	hdr.reserve(30 + name.size());
	appendLe32(hdr, kLocalFileHeaderSig);
	appendLe16(hdr, 20); // version needed
	appendLe16(hdr, nameFlags(name));
	appendLe16(hdr, 0); // compression: store
	appendLe16(hdr, kDosTime);
	appendLe16(hdr, kDosDate);
	appendLe32(hdr, crc);
	appendLe32(hdr, size); // compressed size
	appendLe32(hdr, size); // uncompressed size
	appendLe16(hdr, static_cast<uint16_t>(name.size()));
	appendLe16(hdr, 0); // extra length
	hdr += name;

	if (auto r = writeBytes(hdr); !r) return r;
	if (auto r = writeBytes(data); !r) return r;

	entries_.push_back(Entry{std::move(name), crc, size, offset});
	return {};
}

std::expected<void, std::string> ZipWriter::addFile(const std::filesystem::path& diskPath, std::string_view entryName)
{
	auto r = forge::os::read_file(diskPath.string());
	if (!r) return fail(std::format("Cannot read {}: {}", diskPath.string(), r.err.what));
	return addEntry(entryName, r.value);
}

std::expected<void, std::string> ZipWriter::close()
{
	if (!open_) return fail(error_.empty() ? std::string("ZIP archive is not open") : error_);
	if (closed_) return {};
	closed_ = true;

	const uint32_t centralDirOffset = static_cast<uint32_t>(offset_);
	std::string dir;
	for (const Entry& e: entries_)
	{
		appendLe32(dir, kCentralDirHeaderSig);
		appendLe16(dir, 20); // version made by
		appendLe16(dir, 20); // version needed
		appendLe16(dir, nameFlags(e.name));
		appendLe16(dir, 0); // compression
		appendLe16(dir, kDosTime);
		appendLe16(dir, kDosDate);
		appendLe32(dir, e.crc32);
		appendLe32(dir, e.size);
		appendLe32(dir, e.size);
		appendLe16(dir, static_cast<uint16_t>(e.name.size()));
		appendLe16(dir, 0); // extra
		appendLe16(dir, 0); // comment
		appendLe16(dir, 0); // disk start
		appendLe16(dir, 0); // internal attrs
		appendLe32(dir, 0); // external attrs
		appendLe32(dir, e.localHeaderOffset);
		dir += e.name;
	}

	std::string eocd;
	appendLe32(eocd, kEndOfCentralDirSig);
	appendLe16(eocd, 0); // disk
	appendLe16(eocd, 0); // central dir disk
	appendLe16(eocd, static_cast<uint16_t>(entries_.size()));
	appendLe16(eocd, static_cast<uint16_t>(entries_.size()));
	appendLe32(eocd, static_cast<uint32_t>(dir.size()));
	appendLe32(eocd, centralDirOffset);
	appendLe16(eocd, 0); // comment length

	auto r = writeBytes(dir);
	if (r) r = writeBytes(eocd);
	if (r)
	{
		if (auto st = impl_->file.sync(); !st) r = fail(std::format("Cannot flush {}: {}", path_.string(), st.err.what));
	}
	impl_->file.close();
	open_ = false;
	return r;
}

} // namespace retdec::gui::model
