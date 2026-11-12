/**
 * @file src/gui/model/project_file.cpp
 * @brief .retdec project JSON serialisation and deserialisation.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/project_file.h"

#include "retdec/gui/model/address.h"

#include <forge/json/json.hpp>
#include <forge/os/fs.hpp>

#include <chrono>
#include <cmath>
#include <exception>
#include <format>

namespace retdec::gui::model {

namespace json = forge::json;

namespace {

std::string trimmed(std::string_view s)
{
	std::size_t b = 0, e = s.size();
	auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
	while (b < e && sp(s[b]))
		++b;
	while (e > b && sp(s[e - 1]))
		--e;
	return std::string(s.substr(b, e - b));
}

std::string str(const json::value& o, std::string_view key)
{
	const json::value* v = o.find(key);
	return (v && v->is_str()) ? v->str() : std::string{};
}

int intOr(const json::value& o, std::string_view key, int dflt)
{
	const json::value* v = o.find(key);
	return (v && v->is_num()) ? static_cast<int>(v->num()) : dflt;
}

std::string nowIsoUtc()
{
	const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
	return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

} // namespace

ProjectFile::ProjectFile(std::string binaryPath): binaryPath_(std::move(binaryPath)) {}

// ─── Mutators ────────────────────────────────────────────────────────────────

void ProjectFile::setBinaryPath(std::string p)
{
	binaryPath_ = std::move(p);
	modified_ = true;
}
void ProjectFile::setArch(std::string a)
{
	arch_ = std::move(a);
	modified_ = true;
}
void ProjectFile::setOs(std::string o)
{
	os_ = std::move(o);
	modified_ = true;
}
void ProjectFile::setEntryPoint(uint64_t ep)
{
	entryPoint_ = ep;
	modified_ = true;
}
void ProjectFile::setDecompiledPath(std::string p)
{
	decompiledOutputPath_ = std::move(p);
	modified_ = true;
}
void ProjectFile::setFunctionListFilter(std::string text)
{
	functionListFilter_ = std::move(text);
	modified_ = true;
}
void ProjectFile::setCallGraphDepth(int depth)
{
	callGraphDepth_ = depth;
	modified_ = true;
}

// ─── Stage tracking ──────────────────────────────────────────────────────────

void ProjectFile::setStageStatus(const std::string& stage, const std::string& status)
{
	stages_[stage] = status;
	modified_ = true;
}

std::string ProjectFile::stageStatus(const std::string& stage) const
{
	const auto it = stages_.find(stage);
	return it == stages_.end() ? std::string("pending") : it->second;
}

std::vector<std::string> ProjectFile::stageNames() const
{
	std::vector<std::string> out;
	out.reserve(stages_.size());
	for (const auto& [k, v]: stages_)
		out.push_back(k);
	return out;
}

// ─── Annotations ─────────────────────────────────────────────────────────────

void ProjectFile::setAnnotation(uint64_t address, const Annotation& ann)
{
	annotations_[address] = ann;
	modified_ = true;
}

std::optional<ProjectFile::Annotation> ProjectFile::annotation(uint64_t address) const
{
	const auto it = annotations_.find(address);
	if (it == annotations_.end()) return std::nullopt;
	return it->second;
}

void ProjectFile::clearAnnotation(uint64_t address)
{
	annotations_.erase(address);
	modified_ = true;
}

// ─── JSON ────────────────────────────────────────────────────────────────────

std::string ProjectFile::toJson() const
{
	json::object root;
	root.emplace_back("version", kVersion);
	root.emplace_back("binary", binaryPath_);
	root.emplace_back("arch", arch_);
	root.emplace_back("os", os_);
	// A JSON number, as before (QJsonValue held it as a double too).
	root.emplace_back("entryPoint", static_cast<double>(entryPoint_));
	root.emplace_back("savedAt", nowIsoUtc());
	root.emplace_back("decompiledOutput", decompiledOutputPath_);
	if (!functionListFilter_.empty()) root.emplace_back("functionListFilter", functionListFilter_);
	if (callGraphDepth_ != 0) root.emplace_back("callGraphDepth", callGraphDepth_);

	json::object stages;
	for (const auto& [k, v]: stages_)
		stages.emplace_back(k, v);
	root.emplace_back("stages", std::move(stages));

	json::object anns;
	for (const auto& [addr, a]: annotations_)
	{
		json::object o{
			{"name", a.name},
			{"comment", a.comment},
			{"signatureOverride", a.signatureOverride},
		};
		json::array tags;
		for (const std::string& t: a.tags)
			if (!t.empty()) tags.emplace_back(t);
		if (!tags.empty()) o.emplace_back("tags", std::move(tags));
		anns.emplace_back(std::to_string(addr), std::move(o));
	}
	root.emplace_back("annotations", std::move(anns));
	return json::dump(root);
}

std::expected<void, std::string> ProjectFile::fromJson(std::string_view text)
{
	json::value doc;
	try
	{
		doc = json::parse(text);
	}
	catch (const std::exception& e)
	{
		lastError_ = std::string("JSON parse error: ") + e.what();
		return std::unexpected(lastError_);
	}
	if (!doc.is_obj())
	{
		lastError_ = "Expected JSON object at top level";
		return std::unexpected(lastError_);
	}
	const int ver = intOr(doc, "version", 0);
	if (ver != kVersion)
	{
		lastError_ = std::format("Unsupported project version {}", ver);
		return std::unexpected(lastError_);
	}

	binaryPath_ = str(doc, "binary");
	arch_ = str(doc, "arch");
	os_ = str(doc, "os");
	entryPoint_ = 0;
	if (const json::value* ep = doc.find("entryPoint"))
	{
		if (ep->is_num() && ep->num() > 0)
			entryPoint_ = static_cast<uint64_t>(std::llround(ep->num()));
		else if (ep->is_str())
			entryPoint_ = parseAddress(ep->str()).value_or(0);
	}
	decompiledOutputPath_ = str(doc, "decompiledOutput");
	functionListFilter_ = str(doc, "functionListFilter");
	callGraphDepth_ = intOr(doc, "callGraphDepth", 0);
	savedAt_ = str(doc, "savedAt");

	stages_.clear();
	if (const json::value* st = doc.find("stages"); st && st->is_obj())
		for (const auto& [k, v]: st->obj())
			stages_[k] = v.is_str() ? v.str() : std::string{};

	annotations_.clear();
	if (const json::value* an = doc.find("annotations"); an && an->is_obj())
	{
		for (const auto& [k, v]: an->obj())
		{
			const uint64_t addr = parseAddress(k).value_or(0);
			Annotation ann;
			if (v.is_obj())
			{
				ann.name = str(v, "name");
				ann.comment = str(v, "comment");
				ann.signatureOverride = str(v, "signatureOverride");
				if (const json::value* tags = v.find("tags"); tags && tags->is_arr())
					for (const json::value& t: tags->arr())
					{
						if (!t.is_str()) continue;
						std::string tt = trimmed(t.str());
						if (!tt.empty()) ann.tags.push_back(std::move(tt));
					}
			}
			annotations_[addr] = std::move(ann);
		}
	}

	modified_ = false;
	return {};
}

// ─── I/O ─────────────────────────────────────────────────────────────────────

std::expected<void, std::string> ProjectFile::load(const std::filesystem::path& projectPath)
{
	auto r = forge::os::read_file(projectPath.string());
	if (!r)
	{
		lastError_ = "Cannot open project file: " + r.err.what;
		return std::unexpected(lastError_);
	}
	return fromJson(r.value);
}

std::expected<void, std::string> ProjectFile::save(const std::filesystem::path& projectPath) const
{
	// A failed or short write (full disk, quota, vanished network share) must
	// not clear modified_: the user would close the project believing it saved.
	// write_file_atomic writes a sibling temp file, fsyncs it and renames it
	// over the target, so a failure leaves the previous project intact.
	if (auto st = forge::os::fs::write_file_atomic(projectPath.string(), toJson()); !st)
	{
		lastError_ = "Cannot write project file: " + st.err.what;
		return std::unexpected(lastError_);
	}
	modified_ = false;
	return {};
}

} // namespace retdec::gui::model
