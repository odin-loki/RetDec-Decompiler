/**
 * @file tests/gui/decompiler_launch_test.cpp
 */

#include "retdec/gui/decompiler_launch.h"
#include "retdec/gui/panels/live_console_panel.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>

TEST(DecompilerLaunch, ResolveGuiDecompiledCPathBesideBinary) {
    const QString got = retdec::gui::resolveGuiDecompiledCPath(
            QStringLiteral("C:/bin/sample.exe"), QString());
    EXPECT_EQ(got, QStringLiteral("C:/bin/sample.gui-decompiled.c"));
}

TEST(DecompilerLaunch, ResolveGuiDecompiledCPathCustomDir) {
    const QString got = retdec::gui::resolveGuiDecompiledCPath(
            QStringLiteral("C:/OneDrive/Desktop/foo.exe"),
            QStringLiteral("D:/retdec-out"));
    EXPECT_EQ(got, QStringLiteral("D:/retdec-out/foo.gui-decompiled.c"));
}

TEST(DecompilerLaunch, LocateGuiDecompiledCPathPrefersConfiguredDir) {
    QTemporaryDir outDir;
    QTemporaryDir binDir;
    ASSERT_TRUE(outDir.isValid());
    ASSERT_TRUE(binDir.isValid());
    const QString binPath = binDir.filePath(QStringLiteral("app.exe"));
    QFile bin(binPath);
    ASSERT_TRUE(bin.open(QIODevice::WriteOnly));
    bin.write("MZ");
    bin.close();

    const QString beside = retdec::gui::resolveGuiDecompiledCPath(binPath, QString());
    QFile besideC(beside);
    ASSERT_TRUE(besideC.open(QIODevice::WriteOnly));
    besideC.write("int x;");
    besideC.close();

    const QString configured =
            retdec::gui::resolveGuiDecompiledCPath(binPath, outDir.path());
    QFile configuredC(configured);
    ASSERT_TRUE(configuredC.open(QIODevice::WriteOnly));
    configuredC.write("int y;");
    configuredC.close();

    const QString located = retdec::gui::locateGuiDecompiledCPath(binPath, outDir.path());
    EXPECT_EQ(located, QFileInfo(configured).absoluteFilePath());
}

TEST(DecompilerLaunch, LocateGuiDecompiledCPathFallsBackToBesideBinary) {
    QTemporaryDir binDir;
    ASSERT_TRUE(binDir.isValid());
    const QString binPath = binDir.filePath(QStringLiteral("app.exe"));
    QFile bin(binPath);
    ASSERT_TRUE(bin.open(QIODevice::WriteOnly));
    bin.write("MZ");
    bin.close();

    const QString beside = retdec::gui::resolveGuiDecompiledCPath(binPath, QString());
    QFile besideC(beside);
    ASSERT_TRUE(besideC.open(QIODevice::WriteOnly));
    besideC.write("int x;");
    besideC.close();

    const QString located = retdec::gui::locateGuiDecompiledCPath(
            binPath, QStringLiteral("C:/nonexistent-output-dir"));
    EXPECT_EQ(located, QFileInfo(beside).absoluteFilePath());
}

TEST(DecompilerLaunch, BuildsBaselineArguments) {
    retdec::gui::DecompilerLaunchRequest req;
    req.binaryPath = QStringLiteral("C:/bin/sample.exe");
    req.outputPath = QStringLiteral("C:/bin/sample.gui-decompiled.c");
    req.arch       = QStringLiteral("x86-64");

    const QStringList args = retdec::gui::buildDecompilerArguments(req);
    ASSERT_FALSE(args.isEmpty());
    EXPECT_EQ(args.at(0), req.binaryPath);
    EXPECT_TRUE(args.contains(QStringLiteral("-o")));
    EXPECT_TRUE(args.contains(req.outputPath));
    EXPECT_TRUE(args.contains(QStringLiteral("-f")));
    EXPECT_TRUE(args.contains(QStringLiteral("plain")));
    EXPECT_TRUE(args.contains(QStringLiteral("-s")));
    EXPECT_TRUE(args.contains(QStringLiteral("-a")));
    EXPECT_TRUE(args.contains(QStringLiteral("x86-64")));
    EXPECT_FALSE(args.contains(QStringLiteral("--backend-no-opts")));
    EXPECT_FALSE(args.contains(QStringLiteral("--print-after-all")));
}

TEST(DecompilerLaunch, FastModeAddsFlagsAndLlvmPassesJson) {
    retdec::gui::DecompilerLaunchRequest req;
    req.binaryPath    = QStringLiteral("/tmp/bin");
    req.outputPath    = QStringLiteral("/tmp/out.c");
    req.fastDecompile = true;

    std::unique_ptr<QTemporaryFile> passes;
    const QStringList args =
            retdec::gui::buildDecompilerArguments(req, nullptr, &passes);
    ASSERT_FALSE(args.isEmpty());
    EXPECT_TRUE(args.contains(QStringLiteral("--backend-no-opts")));
    EXPECT_TRUE(args.contains(QStringLiteral("--disable-static-code-detection")));
    EXPECT_TRUE(args.contains(QStringLiteral("--llvm-passes-json")));
    ASSERT_NE(passes, nullptr);
    EXPECT_FALSE(passes->fileName().isEmpty());
}

TEST(DecompilerLaunch, PrintAfterAllFlag) {
    retdec::gui::DecompilerLaunchRequest req;
    req.binaryPath    = QStringLiteral("/tmp/bin");
    req.outputPath    = QStringLiteral("/tmp/out.c");
    req.printAfterAll = true;

    const QStringList args = retdec::gui::buildDecompilerArguments(req);
    EXPECT_TRUE(args.contains(QStringLiteral("--print-after-all")));
}

TEST(DecompilerLaunch, SelectFunctionsFlag) {
    retdec::gui::DecompilerLaunchRequest req;
    req.binaryPath = QStringLiteral("/tmp/bin");
    req.outputPath = QStringLiteral("/tmp/out.c");
    req.selectedFunctions = {QStringLiteral("main"), QStringLiteral("function_401000")};

    const QStringList args = retdec::gui::buildDecompilerArguments(req);
    EXPECT_TRUE(args.contains(QStringLiteral("--select-functions")));
    const int idx = args.indexOf(QStringLiteral("--select-functions"));
    ASSERT_GE(idx, 0);
    EXPECT_LT(idx + 1, args.size());
    EXPECT_EQ(args.at(idx + 1), QStringLiteral("main,function_401000"));
}

TEST(DecompilerLaunch, AppendLogIncrementalFromOffset) {
    QTemporaryFile log;
    log.setAutoRemove(true);
    ASSERT_TRUE(log.open());
    log.write("line one\n");
    log.close();

    qint64 offset = 0;
    retdec::gui::panels::LiveConsolePanel panel;
    EXPECT_TRUE(retdec::gui::appendDecompilerLogIncrementalToConsole(
            &panel, log.fileName(), &offset));
    EXPECT_GT(offset, 0);

    QFile append(log.fileName());
    ASSERT_TRUE(append.open(QIODevice::Append));
    append.write("line two\n");
    append.close();

    EXPECT_TRUE(retdec::gui::appendDecompilerLogIncrementalToConsole(
            &panel, log.fileName(), &offset));
    EXPECT_FALSE(retdec::gui::appendDecompilerLogIncrementalToConsole(
            &panel, log.fileName(), &offset));
}

TEST(DecompilerLaunch, AppendLogTailOnlyForHugeFiles) {
    QTemporaryFile log;
    log.setAutoRemove(true);
    ASSERT_TRUE(log.open());
    const QByteArray body(600 * 1024, 'x');
    ASSERT_EQ(log.write(body), body.size());
    log.close();

    // Smoke: helpers must not crash on a large log path.
    retdec::gui::appendDecompilerLogToConsole(nullptr, log.fileName());
    retdec::gui::scanDecompilerLogDiagnostics(nullptr, log.fileName());
    SUCCEED();
}

TEST(DecompilerLaunch, PollLogProgressReturnsFalseWhenEmpty) {
    QTemporaryFile log;
    log.setAutoRemove(true);
    ASSERT_TRUE(log.open());
    log.close();

    qint64 offset = 0;
    retdec::gui::DecompileLogProgress prog;
    EXPECT_FALSE(retdec::gui::pollDecompileLogProgress(log.fileName(), &offset, &prog));
    EXPECT_EQ(offset, 0);
}

TEST(DecompilerLaunch, PollLogProgressParsesRunningPhase) {
    QTemporaryFile log;
    log.setAutoRemove(true);
    ASSERT_TRUE(log.open());
    log.write("Running phase: Initialization ( 0.01s )\n");
    log.write("Running phase: Input binary to LLVM IR decoding ( 1.23s )\n");
    log.close();

    qint64 offset = 0;
    retdec::gui::DecompileLogProgress prog;
    ASSERT_TRUE(retdec::gui::pollDecompileLogProgress(log.fileName(), &offset, &prog));
    EXPECT_EQ(prog.stage, QStringLiteral("Binary loading"));
    EXPECT_GT(prog.percent, 0);
    EXPECT_LT(prog.percent, 100);

    retdec::gui::DecompileLogProgress prog2;
    EXPECT_FALSE(retdec::gui::pollDecompileLogProgress(log.fileName(), &offset, &prog2));
}

TEST(DecompilerLaunch, PollLogProgressIncrementalRead) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("decompile.log"));

    {
        QFile log(path);
        ASSERT_TRUE(log.open(QIODevice::WriteOnly | QIODevice::Text));
        log.write("Running phase: Initialization ( 0.01s )\n");
    }

    qint64 offset = 0;
    retdec::gui::DecompileLogProgress prog;
    ASSERT_TRUE(retdec::gui::pollDecompileLogProgress(path, &offset, &prog));
    EXPECT_EQ(prog.stage, QStringLiteral("Binary loading"));

    {
        QFile log(path);
        ASSERT_TRUE(log.open(QIODevice::Append | QIODevice::Text));
        log.write("Running phase: Class hierarchy analysis ( 0.50s )\n");
    }

    retdec::gui::DecompileLogProgress prog2;
    ASSERT_TRUE(retdec::gui::pollDecompileLogProgress(path, &offset, &prog2));
    EXPECT_EQ(prog2.stage, QStringLiteral("RTTI reconstruction"));
    EXPECT_GT(prog2.percent, prog.percent);
}

TEST(DecompilerLaunch, PollLogProgressParsesBackendProgressBar) {
    QTemporaryFile log;
    log.setAutoRemove(true);
    ASSERT_TRUE(log.open());
    log.write("[progress] [########----------------] 33% (3/9) elapsed 1.0s, eta 2.0s\n");
    log.close();

    qint64 offset = 0;
    retdec::gui::DecompileLogProgress prog;
    ASSERT_TRUE(retdec::gui::pollDecompileLogProgress(log.fileName(), &offset, &prog));
    EXPECT_EQ(prog.stage, QStringLiteral("Code generation"));
    EXPECT_GE(prog.percent, 65);
    EXPECT_LE(prog.percent, 99);
}
