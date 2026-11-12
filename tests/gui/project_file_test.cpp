/**
 * @file tests/gui/project_file_test.cpp
 */

#include "retdec/gui/model/project_file.h"

#include <forge/json/json.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace m = retdec::gui::model;
namespace fs = std::filesystem;

namespace {

struct TempDir
{
	fs::path path;
	TempDir() { path = fs::path(forge::os::fs::make_temp_dir("retdec-gui-proj-").value); }
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

void writeFile(const fs::path& p, std::string_view data)
{
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	f << data;
}

} // namespace

TEST(ProjectFile, RoundTrip)
{
	TempDir tmp;
	const fs::path path = tmp.path / "p.retdec";

	m::ProjectFile p("/bin/sample.elf");
	EXPECT_FALSE(p.isModified());
	p.setArch("x86_64");
	p.setOs("linux");
	p.setEntryPoint(0x401000);
	p.setDecompiledPath("/out/sample.gui-decompiled.c");
	p.setFunctionListFilter("main");
	p.setCallGraphDepth(3);
	p.setStageStatus("ssa", "done");
	p.setStageStatus("typeInference", "running");
	p.setAnnotation(0x4010a0, {"decrypt_payload", "RC4 decrypt", "void decrypt(char*)", {"crypto", "", "rc4"}});
	p.setAnnotation(0x10, {"tiny", "", "", {}});
	EXPECT_TRUE(p.isModified());

	auto saved = p.save(path);
	ASSERT_TRUE(saved.has_value()) << saved.error();
	EXPECT_FALSE(p.isModified());

	m::ProjectFile q;
	auto loaded = q.load(path);
	ASSERT_TRUE(loaded.has_value()) << loaded.error();
	EXPECT_FALSE(q.isModified());
	EXPECT_EQ(q.binaryPath(), "/bin/sample.elf");
	EXPECT_EQ(q.arch(), "x86_64");
	EXPECT_EQ(q.os(), "linux");
	EXPECT_EQ(q.entryPoint(), 0x401000u);
	EXPECT_EQ(q.decompiledPath(), "/out/sample.gui-decompiled.c");
	EXPECT_EQ(q.functionListFilter(), "main");
	EXPECT_EQ(q.callGraphDepth(), 3);
	EXPECT_EQ(q.stageStatus("ssa"), "done");
	EXPECT_EQ(q.stageStatus("typeInference"), "running");
	EXPECT_EQ(q.stageStatus("never"), "pending");
	EXPECT_EQ(q.stageNames(), (std::vector<std::string>{"ssa", "typeInference"}));
	EXPECT_FALSE(q.savedAt().empty());

	const auto a = q.annotation(0x4010a0);
	ASSERT_TRUE(a.has_value());
	EXPECT_EQ(a->name, "decrypt_payload");
	EXPECT_EQ(a->comment, "RC4 decrypt");
	EXPECT_EQ(a->signatureOverride, "void decrypt(char*)");
	EXPECT_EQ(a->tags, (std::vector<std::string>{"crypto", "rc4"})); // empty tags are not written
	ASSERT_TRUE(q.annotation(0x10).has_value());
	EXPECT_FALSE(q.annotation(0x20).has_value());
	EXPECT_EQ(q.annotations().size(), 2u);
}

TEST(ProjectFile, DocumentMatchesTheQtFormat)
{
	m::ProjectFile p("/b");
	p.setAnnotation(4198560, {"n", "c", "", {}});
	const auto doc = forge::json::parse(p.toJson());
	EXPECT_EQ(doc["version"].i(), 1);
	EXPECT_EQ(doc["binary"].str(), "/b");
	EXPECT_EQ(doc["entryPoint"].i(), 0);
	EXPECT_TRUE(doc["savedAt"].str().ends_with("Z"));
	EXPECT_EQ(doc["savedAt"].str().size(), std::string("2026-03-26T12:00:00Z").size());
	EXPECT_TRUE(doc["stages"].is_obj());
	// Annotation keys are decimal addresses; default session fields are omitted.
	EXPECT_NE(doc["annotations"].find("4198560"), nullptr);
	EXPECT_EQ(doc["annotations"]["4198560"].find("tags"), nullptr);
	EXPECT_EQ(doc.find("functionListFilter"), nullptr);
	EXPECT_EQ(doc.find("callGraphDepth"), nullptr);
}

TEST(ProjectFile, LoadsQtWrittenProjectAndHexKeys)
{
	TempDir tmp;
	const fs::path path = tmp.path / "qt.retdec";
	writeFile(path, R"({
    "version": 1,
    "binary": "C:/samples/a.exe",
    "arch": "x86",
    "os": "windows",
    "entryPoint": 4198400,
    "savedAt": "2026-03-26T12:00:00Z",
    "stages": {"ssa": "done"},
    "annotations": {
        "4198560": {"name": "decrypt_payload", "comment": "RC4 decrypt", "signatureOverride": "", "tags": [" crypto ", 5]},
        "0x401100": {"name": "hex_key"}
    },
    "decompiledOutput": "C:/samples/a.gui-decompiled.c"
})");
	m::ProjectFile p;
	ASSERT_TRUE(p.load(path).has_value()) << p.lastError();
	EXPECT_EQ(p.entryPoint(), 4198400u);
	EXPECT_EQ(p.savedAt(), "2026-03-26T12:00:00Z");
	EXPECT_EQ(p.functionListFilter(), "");
	EXPECT_EQ(p.callGraphDepth(), 0);
	ASSERT_TRUE(p.annotation(4198560).has_value());
	EXPECT_EQ(p.annotation(4198560)->tags, (std::vector<std::string>{"crypto"}));
	ASSERT_TRUE(p.annotation(0x401100).has_value());
	EXPECT_EQ(p.annotation(0x401100)->name, "hex_key");
}

TEST(ProjectFile, ErrorsLeaveAMessage)
{
	TempDir tmp;
	m::ProjectFile p;

	auto missing = p.load(tmp.path / "missing.retdec");
	ASSERT_FALSE(missing.has_value());
	EXPECT_TRUE(missing.error().starts_with("Cannot open project file"));
	EXPECT_EQ(p.lastError(), missing.error());

	writeFile(tmp.path / "bad.retdec", "{ not json");
	auto bad = p.load(tmp.path / "bad.retdec");
	ASSERT_FALSE(bad.has_value());
	EXPECT_TRUE(bad.error().starts_with("JSON parse error"));

	writeFile(tmp.path / "arr.retdec", "[]");
	EXPECT_EQ(p.load(tmp.path / "arr.retdec").error(), "Expected JSON object at top level");

	writeFile(tmp.path / "v2.retdec", R"({"version": 2})");
	EXPECT_EQ(p.load(tmp.path / "v2.retdec").error(), "Unsupported project version 2");

	m::ProjectFile q("/b");
	q.setArch("arm");
	auto unwritable = q.save(tmp.path / "no-such-dir" / "p.retdec");
	ASSERT_FALSE(unwritable.has_value());
	EXPECT_TRUE(unwritable.error().starts_with("Cannot write project file"));
	EXPECT_TRUE(q.isModified()); // a failed save must not look saved
}

TEST(ProjectFile, ModifiedTracking)
{
	m::ProjectFile p;
	p.setStageStatus("x", "done");
	EXPECT_TRUE(p.isModified());
	p.clearModified();
	p.clearAnnotation(0x1);
	EXPECT_TRUE(p.isModified());
	p.clearModified();
	p.setAnnotation(0x1, {});
	EXPECT_TRUE(p.isModified());
	p.clearAnnotation(0x1);
	EXPECT_FALSE(p.annotation(0x1).has_value());
}

TEST(ProjectFile, SaveReplacesAtomically)
{
	TempDir tmp;
	const fs::path path = tmp.path / "p.retdec";
	m::ProjectFile p("/first");
	ASSERT_TRUE(p.save(path).has_value());
	p.setBinaryPath("/second");
	ASSERT_TRUE(p.save(path).has_value());
	m::ProjectFile q;
	ASSERT_TRUE(q.load(path).has_value());
	EXPECT_EQ(q.binaryPath(), "/second");
	// No temp files left behind.
	int files = 0;
	for (const auto& e: fs::directory_iterator(tmp.path))
	{
		(void) e;
		++files;
	}
	EXPECT_EQ(files, 1);
}
