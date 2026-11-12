/**
 * @file tests/gui/code_render_preview_test.cpp
 * @brief Renders each code / console / inspect panel headless with the CPU
 *        rasterizer (draw lists must be non-trivial). With
 *        RETDEC_GUI_TEST_PNG_DIR set, the frames are written there as PNGs
 *        for a visual check.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "code_ui_test_util.h"

#include "retdec/gui/ui/assembly_panel.h"
#include "retdec/gui/ui/binary_browser_panel.h"
#include "retdec/gui/ui/command_log_panel.h"
#include "retdec/gui/ui/decompiled_c_panel.h"
#include "retdec/gui/ui/diagnostics_panel.h"
#include "retdec/gui/ui/inspect_panel.h"
#include "retdec/gui/ui/ir_panel.h"
#include "retdec/gui/ui/live_console_panel.h"
#include "retdec/gui/ui/triage_banner.h"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace retdec::gui;
using retdec::gui::test::HeadlessUi;

namespace {
void finish(HeadlessUi& h, const char* name)
{
	h.frame(3);
	EXPECT_GT(h.ui.drawList().size(), 10u);
	if (const char* dir = std::getenv("RETDEC_GUI_TEST_PNG_DIR")) EXPECT_TRUE(h.writePng(std::string(dir) + "/" + name + ".png"));
}
} // namespace

TEST(CodeRenderPreview, DecompiledC)
{
	HeadlessUi h({900.f, 420.f});
	ui::DecompiledCPanel p(h.ui, h.ui.root());
	p.setSource("#include <stdint.h>\n\n// Address range: 0x401000 - 0x401030\nint32_t main(int argc, char **argv) {\n"
	            "    /* entry */\n    int32_t v = 0x10; // 0x401004\n    puts(\"hello\");\n    return v + argc;\n}\n");
	p.onFunctionSelected(0x401000, "main");
	p.showFindBar();
	p.setFindText("main");
	p.submitFind();
	finish(h, "decompiled_c");
	ui::DecompiledCPanel empty(h.ui, h.ui.root());
}

TEST(CodeRenderPreview, EmptyDecompiledC)
{
	HeadlessUi h({700.f, 300.f});
	ui::DecompiledCPanel p(h.ui, h.ui.root());
	finish(h, "decompiled_c_empty");
}

TEST(CodeRenderPreview, Assembly)
{
	HeadlessUi h({900.f, 360.f});
	ui::AssemblyPanel p(h.ui, h.ui.root());
	p.setAssemblyText("; function: main at 0x401000\n0x401000:   55                push ebp\n0x401001:   89 e5             mov ebp, esp\n"
	                  "0x401003:   e8 f8 00 00 00    call 0x401100 ; helper\n0x401008:   c3                ret\n");
	p.navigateTo(0x401003);
	finish(h, "assembly");
}

TEST(CodeRenderPreview, IR)
{
	HeadlessUi h({900.f, 300.f});
	ui::IRPanel p(h.ui, h.ui.root());
	p.setIRText("define i32 @main(i32 %argc) {\nentry:\n  %0 = add nsw i32 %argc, 1 ; 0x401004\n  ret i32 %0\n}\n");
	finish(h, "ir");
}

TEST(CodeRenderPreview, Console)
{
	HeadlessUi h({900.f, 300.f});
	ui::LiveConsolePanel p(h.ui, h.ui.root());
	p.appendBanner("retdec-decompiler", {"a.out", "-o", "a.c"}, "/work");
	p.appendLine(ui::LiveConsolePanel::Stream::Stdout, "[INFO] Running phase: Input binary to LLVM IR");
	p.appendLine(ui::LiveConsolePanel::Stream::Stdout, "Warning: unknown section .note");
	p.appendLine(ui::LiveConsolePanel::Stream::Stderr, "Error: something failed");
	p.appendFooter("retdec-decompiler", 1, 1234);
	finish(h, "console");
}

TEST(CodeRenderPreview, CommandLogAndDiagnostics)
{
	HeadlessUi h({900.f, 500.f});
	ui::CommandLogPanel log(h.ui, h.ui.root());
	log.appendRun("retdec-fileinfo", {"--json", "a.out"}, "/opt/retdec/bin", 0, 37, "{ }");
	ui::DiagnosticsPanel d(h.ui, h.ui.root());
	d.addMessage(model::Severity::Info, "Loading", "Loaded 12 functions");
	d.addMessage(model::Severity::Warning, "decompiler log", "Unknown section .foo", 0x401000);
	d.addMessage(model::Severity::Error, "semantic", "Decompilation of main failed");
	finish(h, "commandlog_diagnostics");
}

TEST(CodeRenderPreview, BinaryBrowserInspectBanner)
{
	HeadlessUi h({1000.f, 700.f});
	ui::TriageBanner b(h.ui, h.ui.root());
	b.setBinary("/tmp/a.out");
	b.setMetadata("ELF", "x86-64", "Linux", 16384, "UPX");
	ui::InspectPanel ip(h.ui, h.ui.root());
	ip.runFileinfo("/tmp/a.out", "");
	ui::BinaryBrowserPanel bb(h.ui, h.ui.root());
	bb.loadBinary("/tmp/a.out");
	bb.populateFromFileinfo(R"({"sectionTable":{"sections":[{"name":".text","address":"0x401000","offset":"0x0","sizeInFile":"0x20"}]}})");
	finish(h, "browser_inspect_banner");
}
