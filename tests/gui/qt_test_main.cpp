/**
 * @file tests/gui/qt_test_main.cpp
 * @brief Single main() for retdec-gui-tests (avoids duplicate main from per-file mains).
 *
 * Exactly one QApplication exists for the whole process.  Per-file static QApplication
 * instances corrupt Qt (e.g. QThread: Destroyed while thread is still running).
 *
 * Windows debug builds: enables the CRT debug heap with leak dump-on-exit so
 * unfreed allocations show up in the test output.  Set the environment
 * variable RETDEC_GUI_TEST_NO_CRT_LEAK=1 to disable (e.g. when running under
 * Application Verifier / PageHeap, which already tracks allocations).
 */

#include "qt_test_env.h"

#include "retdec/gui/launch_options.h"

#include <QApplication>
#include <QByteArray>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtGlobal>
#include <QSettings>
#include <QString>
#include <QDir>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <vector>

#if defined(_WIN32) && defined(_DEBUG)
#  include <crtdbg.h>
#  define RETDEC_GUI_TESTS_HAS_CRTDBG 1
#else
#  define RETDEC_GUI_TESTS_HAS_CRTDBG 0
#endif

int retdec_gui_test_argc = 0;
char** retdec_gui_test_argv = nullptr;

namespace {

// Keep the QTemporaryDir alive for the lifetime of the process so the
// QSettings on disk are sandboxed and get wiped on exit.
std::atomic<QTemporaryDir*> g_settingsTempDir{nullptr};

void sandboxQSettings() {
    auto* tmp = new QTemporaryDir();
    if (!tmp->isValid()) {
        qWarning("retdec-gui-tests: could not create settings sandbox dir; using system default.");
        delete tmp;
        return;
    }
    g_settingsTempDir.store(tmp);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp->path());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp->path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    // Make sure QSettings("retdec","retdec-gui") used by the GUI doesn't
    // leak into the real user profile under HKCU\Software\retdec on Windows.
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(tmp->path()));
}

void enableCrtDebugHeap() {
    if (qEnvironmentVariableIntValue("RETDEC_GUI_TEST_NO_CRT_LEAK") != 0)
        return;
#if RETDEC_GUI_TESTS_HAS_CRTDBG
    int flag = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    flag |= _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF;
    _CrtSetDbgFlag(flag);
    // Direct leak dumps to stderr so CI captures them.
    _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
}

} // namespace

int main(int argc, char** argv) {
    enableCrtDebugHeap();
    sandboxQSettings();

    static retdec::gui::ParsedLaunchOptions gLaunch;
    gLaunch = retdec::gui::parseLaunchOptions(argc, argv);
    retdec_gui_test_argc = gLaunch.argc;
    retdec_gui_test_argv = gLaunch.argvPtrs.data();
    QApplication app(gLaunch.argc, gLaunch.argvPtrs.data());

    // Copy argv for gtest — InitGoogleTest mutates argc/argv.
    std::vector<std::string> gStore = gLaunch.argStorage;
    std::vector<char*>      gPtrs;
    gPtrs.reserve(gStore.size() + 1u);
    for (auto& s : gStore) gPtrs.push_back(s.data());
    gPtrs.push_back(nullptr);
    int gac = static_cast<int>(gStore.size());
    ::testing::InitGoogleTest(&gac, gPtrs.data());
    const int rc = RUN_ALL_TESTS();

    if (auto* td = g_settingsTempDir.exchange(nullptr))
        delete td;
    return rc;
}
