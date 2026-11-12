/**
 * @file include/retdec/gui/model/inspect_model.h
 * @brief Everything the Inspect panel, the triage banner and the binary
 *        browser compute: running retdec-fileinfo / retdec-unpacker (Forge
 *        processes on worker threads, results polled on the UI thread),
 *        parsing fileinfo JSON (summary rows, hashes, format / arch / OS /
 *        packer, section table), reading file ranges for hex dumps and file
 *        hashing.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_INSPECT_MODEL_H
#define RETDEC_GUI_MODEL_INSPECT_MODEL_H

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace retdec::gui::model {

// ── fileinfo JSON ────────────────────────────────────────────────────────────

/// One section of fileinfo's sectionTable (strings as fileinfo prints them).
struct SectionInfo
{
	std::string name;
	std::string address;    ///< "0x401000"
	std::string sizeInFile; ///< "0x200"
	uint64_t offset = 0;    ///< file offset
	uint64_t addressValue() const;
	uint64_t sizeValue() const;
};

/// What the GUI shows from `retdec-fileinfo --json --verbose`.
struct FileinfoInfo
{
	/// Summary table: hashes first, then the root's scalar fields sorted by
	/// key (max 48 rows), values as text.
	std::vector<std::pair<std::string, std::string>> summaryRows;
	std::string sha256, md5, crc32;
	std::string format;  ///< "fileFormat"
	std::string arch;    ///< "architecture"
	std::string os;      ///< "osOrAbi" (or "Windows" for PE)
	std::string packer;  ///< name of the first tool of type "packer"
	std::vector<SectionInfo> sections;
	std::string prettyJson; ///< indented re-serialisation (for the JSON tab)
};

/// Parses fileinfo JSON; the error says why when it is not a JSON object.
std::expected<FileinfoInfo, std::string> parseFileinfoJson(std::string_view json);

/// First non-empty string value of @p key (case-insensitive) anywhere in the
/// document, searching each object's own keys before nested ones (depth 8).
std::string firstNestedString(std::string_view json, std::string_view key);

/// "SHA-256: …\nMD5: …\nCRC32: …" (only the hashes present), "" when none.
std::string formatHashesForClipboard(const FileinfoInfo& info);

/// Re-indents JSON text (2 spaces). Returns the input unchanged when it does not parse.
std::string prettyPrintJson(std::string_view json);

// ── Files ────────────────────────────────────────────────────────────────────

/// Reads up to @p size bytes at @p offset.
std::expected<std::vector<uint8_t>, std::string> readFileRange(const std::filesystem::path& path, uint64_t offset, uint64_t size);

/// Classic 16-bytes-per-line hex + ASCII dump; line offsets start at @p fileOffset.
std::string formatHexDump(const std::vector<uint8_t>& data, uint64_t fileOffset);

/// "aa bb cc" (lowercase, space separated).
std::string hexSnippet(const std::vector<uint8_t>& data);

/// "512 B", "1.5 KiB", "3.2 MiB", "1.25 GiB"; "—" for <= 0.
std::string humanSize(int64_t bytes);

/// File size, -1 when unreadable.
int64_t fileSize(const std::filesystem::path& path);

/// BLAKE3 of the whole file (lowercase hex).
std::expected<std::string, std::string> blake3File(const std::filesystem::path& path);

/// "<binary without extension>-unpacked" (the unpacker's default output path).
std::string defaultUnpackOutput(std::string_view binaryPath);

// ── Running the tools ────────────────────────────────────────────────────────

enum class InspectTool : uint8_t
{
	Fileinfo,
	Unpacker,
};

/// A finished tool run.
struct ToolRun
{
	InspectTool kind = InspectTool::Fileinfo;
	std::string tool;              ///< "retdec-fileinfo" / "retdec-unpacker"
	std::string executable;        ///< full path that was run
	std::vector<std::string> args; ///< arguments after the executable
	std::string cwd;
	int exitCode = -1;             ///< -1 when the process could not start
	int64_t elapsedMs = 0;
	std::string out, err;
	std::string errorText;         ///< spawn failure text (exitCode -1)
	bool cancelled = false;
	/// Combined stdout + stderr (unpacker) or stdout (fileinfo), capped at 4000 chars.
	std::string outputTail() const;
};

/// Arguments for `retdec-fileinfo` on @p binary.
std::vector<std::string> fileinfoArgs(std::string_view binary);
/// Arguments for `retdec-unpacker`: [-b] <binary> -o <out>.
std::vector<std::string> unpackerArgs(std::string_view binary, std::string_view outPath, bool brute);

/**
 * Runs fileinfo / unpacker children on worker threads (one of each at a
 * time). Starting a tool that is already running cancels the old run.
 * Finished runs are collected on the UI thread with poll().
 */
class InspectModel {
public:
	InspectModel();
	~InspectModel();
	InspectModel(const InspectModel&) = delete;
	InspectModel& operator=(const InspectModel&) = delete;

	/// Starts `fileinfo --json --verbose <binary>`; error when no executable is given.
	std::expected<void, std::string> startFileinfo(std::string_view binary, std::string_view fileinfoExe);
	/// Starts the unpacker; error when it is running already or no executable is given.
	std::expected<void, std::string> startUnpacker(std::string_view binary, std::string_view unpackerExe,
	                                               std::string_view outPath, bool brute);

	bool running(InspectTool t) const;
	bool anyRunning() const { return running(InspectTool::Fileinfo) || running(InspectTool::Unpacker); }

	/// Cancels (kills the process tree) and waits for the worker.
	void cancel(InspectTool t);
	void cancelAll();

	/// Runs that finished since the last call (UI thread).
	std::vector<ToolRun> poll();

	/// Blocks until no tool is running or @p timeoutMs passes (tests / headless).
	bool waitIdle(int timeoutMs);

private:
	struct Worker;
	std::expected<void, std::string> start(InspectTool kind, std::string tool, std::string exe, std::vector<std::string> args);
	std::shared_ptr<Worker> workers_[2];
	struct Shared;
	std::shared_ptr<Shared> shared_;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_INSPECT_MODEL_H
