/**
 * @file tests/gui/console_model_test.cpp
 * @brief Console / command-log / Problems model: classification, ANSI,
 *        UTF-8 repair, line buffer, capped feed, diagnostics store.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/console_diagnostics.h"
#include "retdec/gui/model/console_log.h"

#include <gtest/gtest.h>

#include <thread>

using namespace retdec::gui::model;

TEST(ConsoleLog, ClassifiesLinesLikeTheQtHighlighter)
{
	EXPECT_EQ(classifyConsoleLine("==> [ts] retdec-decompiler a.out"), ConsoleLineKind::Banner);
	EXPECT_EQ(classifyConsoleLine("<== retdec-decompiler finished (exit=0, time=1 ms)"), ConsoleLineKind::Footer);
	EXPECT_EQ(classifyConsoleLine("[INFO] error recovery pass"), ConsoleLineKind::Plain);
	EXPECT_EQ(classifyConsoleLine("[OK] fatal-looking but fine"), ConsoleLineKind::Plain);
	EXPECT_EQ(classifyConsoleLine("Error: cannot open file"), ConsoleLineKind::Error);
	EXPECT_EQ(classifyConsoleLine("FATAL ERROR"), ConsoleLineKind::Error);
	EXPECT_EQ(classifyConsoleLine("process aborted"), ConsoleLineKind::Error);
	EXPECT_EQ(classifyConsoleLine("Warning: unknown section"), ConsoleLineKind::Warning);
	EXPECT_EQ(classifyConsoleLine("option is DEPRECATED"), ConsoleLineKind::Warning);
	EXPECT_EQ(classifyConsoleLine("warning and error"), ConsoleLineKind::Error); // errors win
	EXPECT_EQ(classifyConsoleLine("plain text"), ConsoleLineKind::Plain);
	EXPECT_EQ(classifyConsoleLine(""), ConsoleLineKind::Plain);
}

TEST(ConsoleLog, StripsAnsi)
{
	EXPECT_EQ(stripAnsi("\x1b[31mred\x1b[0m text"), "red text");
	EXPECT_EQ(stripAnsi("\x1b[1;32;40mX"), "X");
	EXPECT_EQ(stripAnsi("\x1b]0;title\x07rest"), "rest");
	EXPECT_EQ(stripAnsi("no escapes"), "no escapes");
	EXPECT_EQ(stripAnsi("lone \x1b"), "lone \x1b");
}

TEST(ConsoleLog, RepairsUtf8)
{
	EXPECT_EQ(sanitizeUtf8("na\xC3\xAFve \xE2\x82\xAC"), "na\xC3\xAFve \xE2\x82\xAC");
	EXPECT_EQ(sanitizeUtf8("\xff\xfe"), "\xEF\xBF\xBD\xEF\xBF\xBD");
	EXPECT_EQ(sanitizeUtf8("\xC3"), "\xEF\xBF\xBD");                 // truncated
	EXPECT_EQ(sanitizeUtf8("\xC0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD"); // overlong
	EXPECT_EQ(sanitizeUtf8("\x01\x02"), "\x01\x02");
}

TEST(ConsoleLog, BannerFooterAndCommandLine)
{
	const auto b = formatBanner("retdec-foo", {"--bar", "two words", "q\"x y"}, "/tmp", "2026-01-01T00:00:00.000");
	ASSERT_EQ(b.size(), 2u);
	EXPECT_EQ(b[0], "==> [2026-01-01T00:00:00.000] retdec-foo --bar \"two words\" \"q\\\"x y\"");
	EXPECT_EQ(b[1], "==>   cwd: /tmp");
	EXPECT_EQ(formatBanner("t", {}, "", "ts").size(), 1u);
	EXPECT_EQ(formatFooter("retdec-foo", 3, 42), "<== retdec-foo finished (exit=3, time=42 ms)");
	EXPECT_EQ(isoTimestampNow().size(), 23u);
}

TEST(ConsoleLog, CommandLogEntry)
{
	const std::string e = formatCommandLogEntry("tool", {"a", "b"}, "/w", 1, 7, "out\n", "TS");
	EXPECT_EQ(e, "[TS] tool\n  cwd: /w\n  exit: 1  time: 7 ms\n  cmd: a b\n  --- output (tail) ---\nout\n\n\n");
	const std::string quiet = formatCommandLogEntry("tool", {}, "/w", 0, 1, "  \n", "TS");
	EXPECT_EQ(quiet.find("output (tail)"), std::string::npos);
}

TEST(ConsoleBuffer, BlockSemantics)
{
	ConsoleBuffer b(100);
	EXPECT_TRUE(b.isEmpty());
	EXPECT_EQ(b.lineCount(), 1u);
	b.append("one\ntw");
	b.append("o\n");
	EXPECT_EQ(b.lineCount(), 3u);
	EXPECT_EQ(b.line(1), "two");
	EXPECT_EQ(b.text(), "one\ntwo\n");
	b.appendLine("three");
	EXPECT_EQ(b.line(2), "three");
	b.appendParagraph("para");
	EXPECT_EQ(b.text(), "one\ntwo\nthree\n\npara");
	b.clear();
	EXPECT_TRUE(b.isEmpty());
	b.appendParagraph("first");
	EXPECT_EQ(b.text(), "first");
}

TEST(ConsoleBuffer, KindsFollowLineContent)
{
	ConsoleBuffer b;
	b.append("all good, then an err");
	EXPECT_EQ(b.kind(0), ConsoleLineKind::Plain);
	b.append("or\n");
	EXPECT_EQ(b.kind(0), ConsoleLineKind::Error);
}

TEST(ConsoleBuffer, CapsLinesAndCountsDropped)
{
	ConsoleBuffer b(1000);
	for (int i = 0; i < 5000; ++i)
		b.append(std::to_string(i) + "\n");
	EXPECT_LE(b.lineCount(), 1000u);
	EXPECT_EQ(b.droppedLines(), 5001u - 1000u);
	EXPECT_EQ(b.line(b.lineCount() - 2), "4999");
	b.setMaxLines(10);
	EXPECT_EQ(b.lineCount(), 10u);
}

TEST(ConsoleFeed, TakeCappedPrefersNewlinesAndUtf8Boundaries)
{
	std::string s = std::string(60, 'a') + "\n" + std::string(60, 'b');
	std::string head = takeCapped(s, 100);
	EXPECT_EQ(head.size(), 61u);
	EXPECT_EQ(s.size(), 60u);
	std::string u;
	for (int i = 0; i < 50; ++i)
		u += "\xE2\x82\xAC"; // 3-byte €
	head = takeCapped(u, 100);
	EXPECT_EQ(head.size() % 3, 0u);
	EXPECT_EQ(head.size() + u.size(), 150u);
	std::string small = "abc";
	EXPECT_EQ(takeCapped(small, 100), "abc");
	EXPECT_TRUE(small.empty());
}

TEST(ConsoleFeed, AttachPushDrainDetach)
{
	ConsoleFeed f;
	const uint64_t a = f.attach("retdec-fileinfo");
	const uint64_t b = f.attach("");
	EXPECT_EQ(f.label(b), "subprocess");
	EXPECT_EQ(f.sourceCount(), 2u);
	f.push(a, ConsoleStream::Stdout, std::string(10'000, 'x'));
	f.push(a, ConsoleStream::Stderr, "err\n");
	f.push(999, ConsoleStream::Stdout, "ignored");
	bool more = false;
	auto chunks = f.drain(4096, more);
	EXPECT_TRUE(more);
	ASSERT_EQ(chunks.size(), 2u);
	EXPECT_EQ(chunks[0].bytes.size(), 4096u);
	EXPECT_EQ(chunks[1].stream, ConsoleStream::Stderr);
	auto rest = f.detach(a);
	ASSERT_EQ(rest.size(), 1u);
	EXPECT_EQ(rest[0].bytes.size(), 10'000u - 4096u);
	EXPECT_FALSE(f.attached(a));
	f.push(b, ConsoleStream::Stdout, "x");
	f.discardPending();
	EXPECT_EQ(f.pendingBytes(), 0u);
}

TEST(ConsoleFeed, ConcurrentProducers)
{
	ConsoleFeed f;
	const uint64_t s = f.attach("t");
	std::vector<std::thread> th;
	for (int t = 0; t < 4; ++t)
		th.emplace_back([&] {
			for (int i = 0; i < 1000; ++i)
				f.push(s, ConsoleStream::Stdout, "0123456789\n");
		});
	for (auto& t: th)
		t.join();
	std::size_t total = 0;
	bool more = true;
	while (more)
		for (auto& c: f.drain(1 << 20, more))
			total += c.bytes.size();
	EXPECT_EQ(total, 4u * 1000u * 11u);
}

// ── Problems ─────────────────────────────────────────────────────────────────

TEST(DiagnosticsStore, EvictsBeyondCap)
{
	DiagnosticsStore m;
	m.setMaxEntries(1000);
	for (int i = 0; i < 5000; ++i)
	{
		Diagnostic d;
		d.severity = Severity::Info;
		d.source = "stress";
		d.message = std::to_string(i);
		m.add(d);
	}
	EXPECT_LE(m.size(), 1000u);
	EXPECT_EQ(m.at(0).message, "4000");
	m.setMaxEntries(5);
	EXPECT_EQ(m.maxEntries(), 100u); // minimum
}

TEST(DiagnosticsStore, FiltersAndTsv)
{
	DiagnosticsStore m;
	m.add({Severity::Info, "Loading binary", "stage", 0, ""});
	m.add({Severity::Warning, "Unknown section", "log", 0x401000, ""});
	m.add({Severity::Error, "Decompiler crashed", "log", 0, "main"});
	EXPECT_EQ(m.visibleRows(SeverityFilter::All, "").size(), 3u);
	EXPECT_EQ(m.visibleRows(SeverityFilter::Info, ""), std::vector<std::size_t>{0});
	EXPECT_EQ(m.visibleRows(SeverityFilter::Warnings, ""), std::vector<std::size_t>{1});
	EXPECT_EQ(m.visibleRows(SeverityFilter::Errors, ""), std::vector<std::size_t>{2});
	// Text filter replaces the severity filter.
	EXPECT_EQ(m.visibleRows(SeverityFilter::Errors, "SECTION"), std::vector<std::size_t>{1});
	EXPECT_TRUE(m.matches(2, SeverityFilter::Errors, ""));
	EXPECT_FALSE(m.matches(2, SeverityFilter::Info, ""));
	EXPECT_EQ(m.tsv({1, 2}), "Warning\tlog\tUnknown section\nError\tlog\tDecompiler crashed");
	EXPECT_EQ(severityLabel(Severity::Warning), "Warning");
	EXPECT_FALSE(severityIcon(Severity::Error).empty());
}

TEST(DiagnosticsStore, LogLinesBecomeDiagnostics)
{
	auto w = diagnosticFromLogLine("Warning: bad jump at 0x401020", "decompiler log");
	ASSERT_TRUE(w);
	EXPECT_EQ(w->severity, Severity::Warning);
	EXPECT_EQ(w->address, 0x401020u);
	EXPECT_EQ(w->source, "decompiler log");
	EXPECT_EQ(diagnosticFromLogLine("error: x")->severity, Severity::Error);
	EXPECT_FALSE(diagnosticFromLogLine("[INFO] error handler"));
	EXPECT_FALSE(diagnosticFromLogLine("all good"));
}
