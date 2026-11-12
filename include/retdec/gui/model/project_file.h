/**
 * @file include/retdec/gui/model/project_file.h
 * @brief ProjectFile — .retdec JSON project serialisation.
 *
 * A retdec project stores:
 *   - path to the original binary
 *   - analysis settings (arch, OS, entry point override)
 *   - per-function user annotations (rename, comment, signature override, tags)
 *   - analysis stage completion status
 *   - decompiled C output path (if saved)
 *
 * File format (JSON), identical to the Qt GUI's so old projects open:
 * ```json
 * {
 *   "version": 1,
 *   "binary": "/path/to/file.elf",
 *   "arch": "x86_64",
 *   "os": "linux",
 *   "entryPoint": 0,
 *   "savedAt": "2026-03-26T12:00:00Z",
 *   "decompiledOutput": "/path/to/output.c",
 *   "functionListFilter": "main",
 *   "callGraphDepth": 0,
 *   "stages": { "ssa": "done", "typeInference": "pending" },
 *   "annotations": {
 *     "4198560": { "name": "decrypt_payload", "comment": "RC4 decrypt", "signatureOverride": "", "tags": ["crypto"] }
 *   }
 * }
 * ```
 * Annotation keys are the address in decimal (as the Qt version wrote them);
 * on load a `0x`-prefixed hex key is accepted too. Optional GUI session fields
 * (`functionListFilter`, `callGraphDepth`) are omitted when default; omitted
 * keys restore defaults on load.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_PROJECT_FILE_H
#define RETDEC_GUI_MODEL_PROJECT_FILE_H

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

class ProjectFile {
public:
	static constexpr int kVersion = 1;

	struct Annotation
	{
		std::string name;
		std::string comment;
		std::string signatureOverride;
		std::vector<std::string> tags;

		bool operator==(const Annotation&) const = default;
	};

	ProjectFile() = default;
	explicit ProjectFile(std::string binaryPath);

	// I/O. On failure lastError() holds the same message.
	std::expected<void, std::string> load(const std::filesystem::path& projectPath);
	/// Atomic write (sibling temp file + rename); clears isModified() on success.
	std::expected<void, std::string> save(const std::filesystem::path& projectPath) const;

	/// The document save() writes (savedAt = now, UTC).
	std::string toJson() const;
	/// Replace the whole project from a document; nothing changes on error.
	std::expected<void, std::string> fromJson(std::string_view json);

	// Accessors
	const std::string& binaryPath() const
	{
		return binaryPath_;
	}
	const std::string& arch() const
	{
		return arch_;
	}
	const std::string& os() const
	{
		return os_;
	}
	uint64_t entryPoint() const
	{
		return entryPoint_;
	}
	const std::string& decompiledPath() const
	{
		return decompiledOutputPath_;
	}
	const std::string& functionListFilter() const
	{
		return functionListFilter_;
	}
	int callGraphDepth() const
	{
		return callGraphDepth_;
	}
	/// "savedAt" of the last load()/fromJson() (empty for a new project).
	const std::string& savedAt() const
	{
		return savedAt_;
	}

	// Mutators (each marks the project modified)
	void setBinaryPath(std::string p);
	void setArch(std::string a);
	void setOs(std::string o);
	void setEntryPoint(uint64_t ep);
	void setDecompiledPath(std::string p);
	void setFunctionListFilter(std::string text);
	void setCallGraphDepth(int depth);

	// Stage tracking
	void setStageStatus(const std::string& stage, const std::string& status);
	/// "pending" for a stage never set.
	std::string stageStatus(const std::string& stage) const;
	/// Sorted stage names.
	std::vector<std::string> stageNames() const;

	// Annotations
	void setAnnotation(uint64_t address, const Annotation& ann);
	std::optional<Annotation> annotation(uint64_t address) const;
	void clearAnnotation(uint64_t address);
	const std::map<uint64_t, Annotation>& annotations() const
	{
		return annotations_;
	}

	bool isModified() const
	{
		return modified_;
	}
	void clearModified()
	{
		modified_ = false;
	}
	const std::string& lastError() const
	{
		return lastError_;
	}

private:
	std::string binaryPath_;
	std::string arch_;
	std::string os_;
	uint64_t entryPoint_ = 0;
	std::string decompiledOutputPath_;
	std::string functionListFilter_;
	int callGraphDepth_ = 0;
	std::string savedAt_;
	std::map<std::string, std::string> stages_;
	std::map<uint64_t, Annotation> annotations_;
	mutable bool modified_ = false;
	mutable std::string lastError_;
};

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_PROJECT_FILE_H
