/**
 * @file src/gui/model/inspect_model.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/inspect_model.h"

#include "retdec/gui/model/code_address.h"

#include <forge/hash/blake3.hpp>
#include <forge/json/json.hpp>
#include <forge/os/process.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>

namespace retdec::gui::model {

namespace {

namespace fj = forge::json;

bool iequals(std::string_view a, std::string_view b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	return true;
}

std::string trimmed(std::string_view s)
{
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
		s.remove_prefix(1);
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
		s.remove_suffix(1);
	return std::string(s);
}

std::optional<fj::value> parseJson(std::string_view text, std::string* err = nullptr)
{
	try
	{
		return fj::parse(text);
	}
	catch (const std::exception& e)
	{
		if (err) *err = e.what();
		return std::nullopt;
	}
}

std::string nestedString(const fj::value& v, std::string_view key, int depth = 0)
{
	if (depth > 8) return {};
	if (v.is_obj())
	{
		for (const auto& [k, x]: v.obj())
		{
			if (!iequals(k, key) || !x.is_str()) continue;
			std::string s = trimmed(x.str());
			if (!s.empty()) return s;
		}
		for (const auto& [k, x]: v.obj())
		{
			std::string s = nestedString(x, key, depth + 1);
			if (!s.empty()) return s;
		}
	}
	else if (v.is_arr())
	{
		for (const auto& x: v.arr())
		{
			std::string s = nestedString(x, key, depth + 1);
			if (!s.empty()) return s;
		}
	}
	return {};
}

/// QString::number(double) style: %g with 6 significant digits.
std::string numberText(double d)
{
	if (std::isnan(d)) return "nan";
	char b[64];
	std::snprintf(b, sizeof b, "%g", d);
	return b;
}

void escape(std::string& o, const std::string& s)
{
	fj::detail::esc(o, s);
}

void pretty(std::string& o, const fj::value& v, int indent)
{
	const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
	const std::string padIn(static_cast<std::size_t>(indent + 1) * 2, ' ');
	if (v.is_obj())
	{
		const auto& ob = v.obj();
		if (ob.empty())
		{
			o += "{}";
			return;
		}
		o += "{\n";
		for (std::size_t i = 0; i < ob.size(); ++i)
		{
			o += padIn;
			escape(o, ob[i].first);
			o += ": ";
			pretty(o, ob[i].second, indent + 1);
			if (i + 1 < ob.size()) o += ',';
			o += '\n';
		}
		o += pad;
		o += '}';
	}
	else if (v.is_arr())
	{
		const auto& ar = v.arr();
		if (ar.empty())
		{
			o += "[]";
			return;
		}
		o += "[\n";
		for (std::size_t i = 0; i < ar.size(); ++i)
		{
			o += padIn;
			pretty(o, ar[i], indent + 1);
			if (i + 1 < ar.size()) o += ',';
			o += '\n';
		}
		o += pad;
		o += ']';
	}
	else
		fj::dump(o, v);
}

} // namespace

// ── SectionInfo / fileinfo parsing ───────────────────────────────────────────

uint64_t SectionInfo::addressValue() const
{
	return parseHexU64(address);
}

uint64_t SectionInfo::sizeValue() const
{
	return parseHexU64(sizeInFile);
}

std::string firstNestedString(std::string_view json, std::string_view key)
{
	const auto v = parseJson(json);
	return v ? nestedString(*v, key) : std::string();
}

std::string prettyPrintJson(std::string_view json)
{
	const auto v = parseJson(json);
	if (!v) return std::string(json);
	std::string o;
	pretty(o, *v, 0);
	o += '\n';
	return o;
}

std::expected<FileinfoInfo, std::string> parseFileinfoJson(std::string_view json)
{
	std::string err;
	const auto parsed = parseJson(json, &err);
	if (!parsed) return std::unexpected(err.empty() ? std::string("invalid JSON") : err);
	if (!parsed->is_obj()) return std::unexpected(std::string("fileinfo output is not a JSON object"));
	const fj::value& root = *parsed;

	FileinfoInfo info;
	info.sha256 = nestedString(root, "sha256");
	info.md5 = nestedString(root, "md5");
	info.crc32 = nestedString(root, "crc32");
	info.format = nestedString(root, "fileFormat");
	info.arch = nestedString(root, "architecture");
	info.os = nestedString(root, "osOrAbi");
	if (info.os.empty() && (info.format.starts_with("PE") || iequals(info.format, "pe"))) info.os = "Windows";
	if (info.os.empty() && info.format.starts_with("Mach-O")) info.os = "macOS";

	if (const fj::value* tools = root.find("tools"); tools && tools->is_arr())
		for (const auto& t: tools->arr())
			if (t.is_obj() && iequals(t.str_or("type", ""), "packer"))
			{
				info.packer = t.str_or("name", "");
				if (!info.packer.empty()) break;
			}

	// Summary rows.
	constexpr std::size_t kMaxRows = 48;
	auto addRow = [&](std::string k, std::string v) {
		if (info.summaryRows.size() >= kMaxRows || v.empty()) return;
		info.summaryRows.emplace_back(std::move(k), std::move(v));
	};
	addRow("sha256", info.sha256);
	addRow("md5", info.md5);
	addRow("crc32", info.crc32);
	std::vector<const std::pair<std::string, fj::value>*> keys;
	for (const auto& kv: root.obj())
		keys.push_back(&kv);
	std::stable_sort(keys.begin(), keys.end(), [](auto* a, auto* b) { return a->first < b->first; });
	for (const auto* kv: keys)
	{
		if (info.summaryRows.size() >= kMaxRows) break;
		const std::string& k = kv->first;
		if (iequals(k, "sha256") || iequals(k, "md5") || iequals(k, "crc32")) continue;
		const fj::value& v = kv->second;
		if (v.is_obj() || v.is_arr()) continue;
		std::string val;
		if (v.is_str())
			val = v.str();
		else if (v.is_num())
			val = numberText(v.num());
		else if (v.is_bool())
			val = v.b() ? "true" : "false";
		else if (v.is_null())
			val = "null";
		addRow(k, std::move(val));
	}

	// Sections.
	if (const fj::value* st = root.find("sectionTable"); st && st->is_obj())
		if (const fj::value* secs = st->find("sections"); secs && secs->is_arr())
			for (const auto& s: secs->arr())
			{
				if (!s.is_obj()) continue;
				SectionInfo si;
				si.name = s.str_or("name", "");
				si.address = s.str_or("address", "");
				si.sizeInFile = s.str_or("sizeInFile", "");
				si.offset = parseHexU64(s.str_or("offset", ""));
				info.sections.push_back(std::move(si));
			}

	pretty(info.prettyJson, root, 0);
	info.prettyJson += '\n';
	return info;
}

std::string formatHashesForClipboard(const FileinfoInfo& info)
{
	std::string out;
	auto add = [&](std::string_view label, const std::string& v) {
		if (v.empty()) return;
		if (!out.empty()) out += '\n';
		out += std::format("{}: {}", label, v);
	};
	add("SHA-256", info.sha256);
	add("MD5", info.md5);
	add("CRC32", info.crc32);
	return out;
}

// ── Files ────────────────────────────────────────────────────────────────────

std::expected<std::vector<uint8_t>, std::string>
readFileRange(const std::filesystem::path& path, uint64_t offset, uint64_t size)
{
	std::FILE* f = std::fopen(path.string().c_str(), "rb");
	if (!f) return std::unexpected(std::format("Cannot open binary: {}", path.string()));
	struct Closer
	{
		std::FILE* f;
		~Closer()
		{
			std::fclose(f);
		}
	} closer{f};
#if defined(_WIN32)
	if (_fseeki64(f, static_cast<long long>(offset), SEEK_SET) != 0)
#else
	if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0)
#endif
		return std::unexpected(std::format("Cannot seek to offset 0x{:x}", offset));
	std::vector<uint8_t> out(static_cast<std::size_t>(size));
	const std::size_t n = size ? std::fread(out.data(), 1, out.size(), f) : 0;
	if (std::ferror(f)) return std::unexpected(std::format("Cannot read {}", path.string()));
	out.resize(n);
	return out;
}

std::string formatHexDump(const std::vector<uint8_t>& data, uint64_t fileOffset)
{
	if (data.empty()) return {};
	std::string out;
	out.reserve(data.size() * 4 + 16);
	constexpr std::size_t kBytesPerLine = 16;
	for (std::size_t i = 0; i < data.size(); i += kBytesPerLine)
	{
		const std::size_t lineLen = std::min(kBytesPerLine, data.size() - i);
		out += std::format("{:08x}  ", fileOffset + i);
		for (std::size_t j = 0; j < kBytesPerLine; ++j)
		{
			if (j < lineLen)
				out += std::format("{:02x} ", data[i + j]);
			else
				out += "   ";
			if (j == 7) out += ' ';
		}
		out += ' ';
		for (std::size_t j = 0; j < lineLen; ++j)
		{
			const uint8_t c = data[i + j];
			out += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
		}
		out += '\n';
	}
	return out;
}

std::string hexSnippet(const std::vector<uint8_t>& data)
{
	std::string out;
	out.reserve(data.size() * 3);
	for (std::size_t i = 0; i < data.size(); ++i)
	{
		if (i) out += ' ';
		out += std::format("{:02x}", data[i]);
	}
	return out;
}

std::string humanSize(int64_t bytes)
{
	if (bytes <= 0) return "\xE2\x80\x94"; // —
	const double kib = 1024.0;
	const double b = static_cast<double>(bytes);
	if (b < kib) return std::format("{} B", bytes);
	if (b < kib * kib) return std::format("{:.1f} KiB", b / kib);
	if (b < kib * kib * kib) return std::format("{:.1f} MiB", b / (kib * kib));
	return std::format("{:.2f} GiB", b / (kib * kib * kib));
}

int64_t fileSize(const std::filesystem::path& path)
{
	std::error_code ec;
	const auto n = std::filesystem::file_size(path, ec);
	return ec ? -1 : static_cast<int64_t>(n);
}

std::expected<std::string, std::string> blake3File(const std::filesystem::path& path)
{
	std::FILE* f = std::fopen(path.string().c_str(), "rb");
	if (!f) return std::unexpected(std::format("Cannot open {}", path.string()));
	forge::hash::blake3 h;
	std::vector<uint8_t> buf(1 << 16);
	std::size_t n;
	while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
		h.update(buf.data(), n);
	const bool err = std::ferror(f) != 0;
	std::fclose(f);
	if (err) return std::unexpected(std::format("Cannot read {}", path.string()));
	return forge::hash::blake3::hex(h.final());
}

std::string defaultUnpackOutput(std::string_view binaryPath)
{
	if (binaryPath.empty()) return {};
	std::string base(binaryPath);
	const std::size_t dot = base.rfind('.');
	const std::size_t sep = base.find_last_of("/\\");
	if (dot != std::string::npos && dot > 0 && (sep == std::string::npos || dot > sep + 1)) base.resize(dot);
	return base + "-unpacked";
}

// ── Tool runs ────────────────────────────────────────────────────────────────

std::string ToolRun::outputTail() const
{
	std::string text = kind == InspectTool::Unpacker ? out + err : out;
	if (exitCode == -1 && text.empty()) text = errorText;
	if (text.size() > 4000)
	{
		std::size_t cut = 4000;
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
			--cut;
		text.resize(cut);
		text += "\n\xE2\x80\xA6"; // …
	}
	return text;
}

std::vector<std::string> fileinfoArgs(std::string_view binary)
{
	return {"--json", "--verbose", std::string(binary)};
}

std::vector<std::string> unpackerArgs(std::string_view binary, std::string_view outPath, bool brute)
{
	std::vector<std::string> a;
	if (brute) a.emplace_back("-b");
	a.emplace_back(binary);
	a.emplace_back("-o");
	a.emplace_back(outPath);
	return a;
}

struct InspectModel::Shared
{
	std::mutex mutex;
	std::vector<ToolRun> done;
};

struct InspectModel::Worker
{
	std::jthread thread;
	std::atomic<bool> finished{false};
};

InspectModel::InspectModel(): shared_(std::make_shared<Shared>()) {}

InspectModel::~InspectModel()
{
	cancelAll();
}

bool InspectModel::running(InspectTool t) const
{
	const auto& w = workers_[static_cast<int>(t)];
	return w && !w->finished.load();
}

void InspectModel::cancel(InspectTool t)
{
	auto& w = workers_[static_cast<int>(t)];
	if (!w) return;
	w->thread.request_stop();
	if (w->thread.joinable()) w->thread.join();
	w.reset();
}

void InspectModel::cancelAll()
{
	cancel(InspectTool::Fileinfo);
	cancel(InspectTool::Unpacker);
}

std::expected<void, std::string>
InspectModel::start(InspectTool kind, std::string tool, std::string exe, std::vector<std::string> args)
{
	auto w = std::make_shared<Worker>();
	std::string cwd;
	{
		const std::filesystem::path p(exe);
		if (p.has_parent_path())
		{
			std::error_code ec;
			cwd = std::filesystem::absolute(p.parent_path(), ec).string();
		}
	}
	std::shared_ptr<Shared> shared = shared_;
	Worker* raw = w.get();
	w->thread = std::jthread([kind, tool, exe, args, cwd, shared, raw](std::stop_token st) {
		ToolRun run;
		run.kind = kind;
		run.tool = tool;
		run.executable = exe;
		run.args = args;
		run.cwd = cwd;
		forge::os::spawn_spec sp;
		sp.argv.push_back(exe);
		sp.argv.insert(sp.argv.end(), args.begin(), args.end());
		sp.cwd = cwd;
		sp.stop = st;
		const auto t0 = std::chrono::steady_clock::now();
		auto r = forge::os::spawn_wait(sp);
		run.elapsedMs =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		if (!r)
		{
			run.exitCode = -1;
			run.errorText = r.err.what.empty() ? std::string("failed to start") : r.err.what;
		}
		else
		{
			run.exitCode = r->exit_code;
			run.out = std::move(r->out);
			run.err = std::move(r->err);
			run.cancelled = r->cancelled;
		}
		if (!run.cancelled)
		{
			std::lock_guard lock(shared->mutex);
			shared->done.push_back(std::move(run));
		}
		raw->finished.store(true);
	});
	workers_[static_cast<int>(kind)] = std::move(w);
	return {};
}

std::expected<void, std::string> InspectModel::startFileinfo(std::string_view binary, std::string_view fileinfoExe)
{
	if (fileinfoExe.empty()) return std::unexpected(std::string("retdec-fileinfo not found (install or add to PATH)."));
	if (binary.empty()) return std::unexpected(std::string("No binary loaded."));
	cancel(InspectTool::Fileinfo);
	return start(InspectTool::Fileinfo, "retdec-fileinfo", std::string(fileinfoExe), fileinfoArgs(binary));
}

std::expected<void, std::string>
InspectModel::startUnpacker(std::string_view binary, std::string_view unpackerExe, std::string_view outPath, bool brute)
{
	if (unpackerExe.empty()) return std::unexpected(std::string("retdec-unpacker not found."));
	if (binary.empty()) return std::unexpected(std::string("No binary loaded."));
	if (outPath.empty()) return std::unexpected(std::string("Set an unpack output path."));
	if (running(InspectTool::Unpacker)) return std::unexpected(std::string("retdec-unpacker is already running."));
	cancel(InspectTool::Unpacker);
	return start(
		InspectTool::Unpacker, "retdec-unpacker", std::string(unpackerExe), unpackerArgs(binary, outPath, brute));
}

std::vector<ToolRun> InspectModel::poll()
{
	for (auto& w: workers_)
		if (w && w->finished.load())
		{
			if (w->thread.joinable()) w->thread.join();
			w.reset();
		}
	std::lock_guard lock(shared_->mutex);
	std::vector<ToolRun> out;
	out.swap(shared_->done);
	return out;
}

bool InspectModel::waitIdle(int timeoutMs)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (anyRunning())
	{
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return true;
}

} // namespace retdec::gui::model
