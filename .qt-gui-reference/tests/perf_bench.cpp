/**
 * @file tests/gui/perf_bench.cpp
 * @brief Standalone GUI performance benchmark.
 *
 * Two scenarios:
 *
 *  1) `--scenario live-stream FILE` — replays the captured decompiler log
 *     into a LiveConsolePanel using realistic chunk sizes (a few KB) and
 *     measures the worst per-frame insertion latency.
 *
 *  2) `--scenario decompiled-load FILE` — single-shot loads a captured
 *     decompiled .c file via DecompiledCPanel::setSource and times it.
 *
 * Output is one JSON object per scenario on stdout so CI can compare runs.
 */

#include "retdec/gui/panels/live_console_panel.h"
#include "retdec/gui/panels/decompiled_c_panel.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QString>
#include <QStringList>
#include <QTextDocument>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <vector>

using namespace retdec::gui::panels;

namespace {

QByteArray readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

int runLiveStream(const QString& path, int chunkSize, bool useStderr) {
    const QByteArray all = readAll(path);
    if (all.isEmpty()) {
        std::fprintf(stderr, "perf-bench: input empty or missing: %s\n",
                     qPrintable(path));
        return 2;
    }

    LiveConsolePanel panel;
    panel.setFlushHz(50);                 // pretend the GUI runs at 50 Hz
    panel.setMaxBytesPerFlush(256 * 1024); // production default
    panel.setMaxBlocks(50'000);

    qint64 worstChunkUs = 0;
    qint64 totalChunkUs = 0;
    int    chunks       = 0;
    // Track latencies above the 60 Hz frame budget (16 ms = a visible drop).
    int    stallFrames  = 0;
    qint64 sumStallUs   = 0;

    const auto stream = useStderr ? LiveConsolePanel::Stream::Stderr
                                  : LiveConsolePanel::Stream::Stdout;

    QElapsedTimer wall;
    wall.start();
    for (int pos = 0; pos < all.size(); pos += chunkSize) {
        const int n = std::min(chunkSize, static_cast<int>(all.size()) - pos);
        QElapsedTimer t;
        t.start();
        panel.appendChunk(stream, all.mid(pos, n));
        const qint64 us = t.nsecsElapsed() / 1000;
        worstChunkUs = std::max(worstChunkUs, us);
        totalChunkUs += us;
        if (us > 16'000) {
            ++stallFrames;
            sumStallUs += us;
        }
        ++chunks;

        // Pump the event loop a little — mirrors how the real GUI runs.
        if ((chunks & 0x3F) == 0)
            QApplication::processEvents();
    }
    QApplication::processEvents();
    const qint64 wallMs = wall.elapsed();

    QJsonObject j;
    j["scenario"]             = useStderr ? "live-stream-stderr" : "live-stream-stdout";
    j["input"]                = path;
    j["input_bytes"]          = static_cast<qint64>(all.size());
    j["chunks"]               = chunks;
    j["chunk_size"]           = chunkSize;
    j["wall_ms"]              = wallMs;
    j["worst_chunk_us"]       = worstChunkUs;
    j["avg_chunk_us"]         = chunks ? totalChunkUs / chunks : 0;
    j["stall_frames_gt16ms"]  = stallFrames;
    j["sum_stall_us"]         = sumStallUs;
    j["throughput_MiB_per_s"] = (wallMs > 0)
            ? (static_cast<double>(all.size()) * 1000.0)
              / (1024.0 * 1024.0 * static_cast<double>(wallMs))
            : 0.0;
    std::cout << QJsonDocument(j).toJson(QJsonDocument::Compact).constData()
              << "\n";
    return 0;
}

/**
 * Production-path bench: instead of calling appendChunk directly, simulate
 * what QProcess does — bytes arrive in readyRead chunks, the LiveConsole's
 * own 16-ms flush timer drains them. We measure the worst single event-loop
 * iteration time (proxy for perceived UI stall) by sampling the wall clock
 * around `QApplication::processEvents()` calls.
 */
int runViaFlush(const QString& path, int chunkSize, bool useStderr) {
    const QByteArray all = readAll(path);
    if (all.isEmpty()) {
        std::fprintf(stderr, "perf-bench: input empty or missing: %s\n",
                     qPrintable(path));
        return 2;
    }

    LiveConsolePanel panel;
    panel.setFlushHz(60);
    panel.setMaxBytesPerFlush(16 * 1024);

    // Use a QProcess proxy to drive the per-process pendingOut buffer.
    // We can't actually pipe bytes into a real QProcess from outside, so we
    // emulate the flush path by writing directly into the live console at
    // the chunk cadence the production code would deliver them, but never
    // exceeding the per-flush cap in a single sync call.
    qint64 worstTickUs = 0;
    qint64 totalTickUs = 0;
    int    ticks       = 0;
    int    stallFrames = 0;

    QElapsedTimer wall;
    wall.start();
    const auto stream = useStderr ? LiveConsolePanel::Stream::Stderr
                                  : LiveConsolePanel::Stream::Stdout;
    for (int pos = 0; pos < all.size(); pos += chunkSize) {
        const int n = std::min(chunkSize, static_cast<int>(all.size()) - pos);
        // Mirror flushAll: split a too-large incoming chunk into per-cap pieces.
        int subPos = 0;
        while (subPos < n) {
            const int take = std::min(16 * 1024, n - subPos);
            QElapsedTimer t;
            t.start();
            panel.appendChunk(stream, all.mid(pos + subPos, take));
            QApplication::processEvents();
            const qint64 us = t.nsecsElapsed() / 1000;
            worstTickUs = std::max(worstTickUs, us);
            totalTickUs += us;
            if (us > 16'000) ++stallFrames;
            ++ticks;
            subPos += take;
        }
    }
    const qint64 wallMs = wall.elapsed();

    // Report the final editor block count to confirm scrollback eviction
    // actually capped memory growth.
    auto* editor = panel.findChild<QPlainTextEdit*>();
    const int finalBlocks = editor ? editor->blockCount() : -1;
    QJsonObject j;
    j["scenario"]            = "via-flush";
    j["input"]               = path;
    j["input_bytes"]         = static_cast<qint64>(all.size());
    j["ticks"]               = ticks;
    j["chunk_size"]          = chunkSize;
    j["wall_ms"]             = wallMs;
    j["worst_tick_us"]       = worstTickUs;
    j["avg_tick_us"]         = ticks ? totalTickUs / ticks : 0;
    j["stall_frames_gt16ms"] = stallFrames;
    j["final_blocks"]        = finalBlocks;
    j["max_blocks_cap"]      = panel.maxBlocks();
    j["throughput_MiB_per_s"] = (wallMs > 0)
            ? (static_cast<double>(all.size()) * 1000.0)
              / (1024.0 * 1024.0 * static_cast<double>(wallMs))
            : 0.0;
    std::cout << QJsonDocument(j).toJson(QJsonDocument::Compact).constData() << "\n";
    return 0;
}

int runDecompiledLoad(const QString& path) {
    const QByteArray all = readAll(path);
    if (all.isEmpty()) {
        std::fprintf(stderr, "perf-bench: input empty or missing: %s\n",
                     qPrintable(path));
        return 2;
    }
    DecompiledCPanel panel;

    QElapsedTimer t;
    t.start();
    panel.setSource(QString::fromUtf8(all));
    const qint64 setSourceMs = t.elapsed();

    // Drive the async-load timer chain to completion, sampling event-loop
    // tick durations. The user perceives responsiveness via these ticks.
    qint64 worstTickUs = 0;
    int    ticks       = 0;
    int    stallFrames = 0;
    auto*  editor      = panel.findChild<QPlainTextEdit*>();
    const qsizetype targetChars = QString::fromUtf8(all).size();
    while (editor && editor->document()->characterCount() - 1 < targetChars) {
        QElapsedTimer tk;
        tk.start();
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        const qint64 us = tk.nsecsElapsed() / 1000;
        worstTickUs = std::max(worstTickUs, us);
        if (us > 16'000) ++stallFrames;
        ++ticks;
        if (ticks > 100'000) break; // safety
    }
    const qint64 totalMs = t.elapsed();

    QJsonObject j;
    j["scenario"]      = "decompiled-load";
    j["input"]         = path;
    j["input_bytes"]   = static_cast<qint64>(all.size());
    j["set_source_ms"] = setSourceMs;
    j["total_ms"]      = totalMs;
    j["ticks"]         = ticks;
    j["worst_tick_us"] = worstTickUs;
    j["stall_frames_gt16ms"] = stallFrames;
    std::cout << QJsonDocument(j).toJson(QJsonDocument::Compact).constData()
              << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCommandLineParser p;
    p.setApplicationDescription("RetDec GUI perf benchmark.");
    p.addHelpOption();
    QCommandLineOption scenarioOpt(
            QStringList{"scenario", "s"},
            "live-stream | decompiled-load",
            "name");
    QCommandLineOption inputOpt(
            QStringList{"input", "i"},
            "Input file (captured stdout/stderr log or decompiled .c).",
            "path");
    QCommandLineOption chunkOpt(
            QStringList{"chunk"},
            "Per-call chunk size in bytes (default 4096).",
            "bytes",
            "4096");
    QCommandLineOption stderrOpt(
            QStringList{"stderr"},
            "Feed the stream as stderr (exercises the [stderr] prefix path).");
    p.addOption(scenarioOpt);
    p.addOption(inputOpt);
    p.addOption(chunkOpt);
    p.addOption(stderrOpt);
    p.process(app);

    if (!p.isSet(scenarioOpt) || !p.isSet(inputOpt)) {
        std::fprintf(stderr, "usage: perf_bench --scenario {live-stream|decompiled-load} --input PATH [--chunk N] [--stderr]\n");
        return 2;
    }
    const QString s        = p.value(scenarioOpt);
    const QString f        = p.value(inputOpt);
    const int     chunk    = p.value(chunkOpt).toInt();
    const bool    isStderr = p.isSet(stderrOpt);
    if (s == "live-stream")        return runLiveStream(f, chunk > 0 ? chunk : 4096, isStderr);
    if (s == "via-flush")          return runViaFlush(f, chunk > 0 ? chunk : 4096, isStderr);
    if (s == "decompiled-load")    return runDecompiledLoad(f);
    std::fprintf(stderr, "unknown scenario: %s\n", qPrintable(s));
    return 2;
}
