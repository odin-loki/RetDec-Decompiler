/**
 * @file include/retdec/gui/model/zip_writer.h
 * @brief Minimal PKZIP writer (store-only, no zlib / external zip library).
 *
 * Writes compression method 0 (store), which every unzip tool reads. Entry
 * names use '/' (backslashes are converted) and must not start with '/'. No
 * ZIP64: an archive is limited to 65535 entries and 4 GiB.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_ZIP_WRITER_H
#define RETDEC_GUI_MODEL_ZIP_WRITER_H

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

class ZipWriter {
public:
	/// Create / truncate @p zipPath. Check isOpen() (errorString() says why not).
	explicit ZipWriter(const std::filesystem::path& zipPath);
	/// Finalises the archive if close() was not called.
	~ZipWriter();

	ZipWriter(const ZipWriter&) = delete;
	ZipWriter& operator=(const ZipWriter&) = delete;

	bool isOpen() const
	{
		return open_;
	}

	/// Add an on-disk file under @p entryName (forward slashes, no leading slash).
	std::expected<void, std::string> addFile(const std::filesystem::path& diskPath, std::string_view entryName);

	/// Add in-memory bytes under @p entryName.
	std::expected<void, std::string> addEntry(std::string_view entryName, std::string_view data);

	/// Write the central directory and close the archive.
	std::expected<void, std::string> close();

	/// Last error message (also returned by the failing call).
	const std::string& errorString() const
	{
		return error_;
	}

	/// Names added so far, in order.
	std::vector<std::string> entryNames() const;

	/// CRC-32 (IEEE 802.3, as PKZIP uses).
	static uint32_t crc32(std::string_view data);

private:
	struct Entry
	{
		std::string name;
		uint32_t crc32 = 0;
		uint32_t size = 0;
		uint32_t localHeaderOffset = 0;
	};
	struct Impl;

	std::expected<void, std::string> fail(std::string msg);
	std::expected<void, std::string> writeBytes(std::string_view bytes);

	std::filesystem::path path_;
	std::unique_ptr<Impl> impl_;
	uint64_t offset_ = 0;
	bool open_ = false;
	bool closed_ = false;
	std::string error_;
	std::vector<Entry> entries_;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_ZIP_WRITER_H
