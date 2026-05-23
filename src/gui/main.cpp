/**
 * @file src/gui/main.cpp
 * @brief RetDec GUI application entry point.
 *
 * Responsibilities:
 *   1. Create QApplication with high-DPI settings.
 *   2. Load settings and apply theme QSS (Catppuccin Mocha; Light attempted).
 *   3. Register bundled monospace fonts (JetBrains Mono / Cascadia Code).
 *   4. Construct and show RetDecMainWindow.
 *   5. Handle command-line argument: optional binary path to open on launch.
 */

#include "retdec/gui/launch_options.h"
#include "retdec/gui/mainwindow.h"
#include "retdec/gui/artifact_loader.h"
#include "retdec/gui/decompiler_launch.h"
#include "retdec/gui/settings/settings.h"
#include "retdec/gui/theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QString>
#include <QStyleFactory>
#include <QTimer>

#include <cstdio>

static void registerFonts() {
    // Bundled fonts may be embedded as Qt resources; if unavailable the system
    // monospace fallback (Consolas / DejaVu Sans Mono) will be used via QSS.
    const QStringList fontResources = {
        ":/fonts/JetBrainsMono-Regular.ttf",
        ":/fonts/JetBrainsMono-Bold.ttf",
        ":/fonts/CascadiaCode.ttf",
    };
    for (const auto& path : fontResources) {
        if (QFile::exists(path))
            QFontDatabase::addApplicationFont(path);
    }
}

int main(int argc, char* argv[]) {
    Q_INIT_RESOURCE(resources);
    // High-DPI support (Qt6 enables it by default, but we are explicit).
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    static retdec::gui::ParsedLaunchOptions gLaunch;
    gLaunch = retdec::gui::parseLaunchOptions(argc, argv);

    QApplication app(gLaunch.argc, gLaunch.argvPtrs.data());
    app.setApplicationName("RetDec");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("retdec");
    app.setWindowIcon(QIcon(":/retdec/icon.png"));

    // Set Fusion as the base style so our QSS overrides cleanly on all platforms.
    app.setStyle(QStyleFactory::create("Fusion"));

    registerFonts();

    retdec::gui::AppSettings::instance().load();
    retdec::gui::applyThemeFromSettings(app);

    // Command-line parsing.
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Retargetable Decompiler GUI.\n"
        "Headless / CI: set RETDEC_GUI_HEADLESS=1 or pass --headless (uses Qt offscreen; no windows).\n"
        "Optional --headless-exit-ms N quits after N ms (automated smoke / debug).");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("binary", "Binary or .retdec project file to open");
    parser.process(app);

    retdec::gui::RetDecMainWindow window;
    window.show();

    const QStringList positional = parser.positionalArguments();
    if (gLaunch.headlessDecompile) {
        if (positional.isEmpty()) {
            std::fprintf(stderr, "retdec-gui: --headless-decompile requires a binary path\n");
            return 2;
        }
        window.setFastDecompilePreset(gLaunch.fastDecompile);
        window.enableQuitWhenDecompileFinishes(true);
    }

    if (!positional.isEmpty()) {
        const QString& arg = positional.first();
        if (gLaunch.headlessDecompile) {
            const QString outDir =
                    retdec::gui::AppSettings::instance().decompiler.decompileOutputDir;
            const QString cPath =
                    retdec::gui::resolveGuiDecompiledCPath(arg, outDir);
            const auto paths = retdec::gui::pathsFromOutputC(cPath);
            QFile::remove(cPath);
            QFile::remove(paths.configPath);
        }
        if (arg.endsWith(".retdec", Qt::CaseInsensitive))
            window.openProject(arg);
        else
            window.openBinary(arg);
    }

    if (gLaunch.headlessDecompile) {
        QTimer::singleShot(100, &window, &retdec::gui::RetDecMainWindow::runDecompileForBenchmark);
    } else if (gLaunch.headlessExitMs > 0) {
        QTimer::singleShot(gLaunch.headlessExitMs, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
