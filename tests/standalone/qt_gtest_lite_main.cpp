/**
 * @file tests/standalone/qt_gtest_lite_main.cpp
 * @brief `main` for GUI suites: one QApplication, then the GoogleTest shim.
 *
 * The GUI tests construct widgets, so they need a QApplication -- and exactly
 * one, for the whole process: a second instance corrupts Qt and shows up as
 * "QThread: Destroyed while thread is still running" or worse. The default
 * gtest_lite_main.cpp has no idea about Qt, so the GUI suite gets this instead.
 *
 * tests/gui/qt_test_env.h declares retdec_gui_test_argc/argv extern; the fixtures
 * pass them to widgets that want the process arguments. They are defined here so
 * a suite built by scripts/standalone_check.sh resolves them the same way one
 * built by tests/gui/CMakeLists.txt does.
 *
 * QSettings is redirected into a temporary directory so a test run cannot read
 * or write the developer's real GUI settings, matching what the CMake harness
 * does. QTemporaryDir removes it on destruction, which happens after
 * RUN_ALL_TESTS returns.
 */

#include "gtest/gtest.h"

#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

int retdec_gui_test_argc = 0;
char** retdec_gui_test_argv = nullptr;

int main(int argc, char** argv)
{
	QTemporaryDir settingsDir;
	if (settingsDir.isValid())
	{
		QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
		QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
		QSettings::setDefaultFormat(QSettings::IniFormat);
		qputenv("XDG_CONFIG_HOME", QFile::encodeName(settingsDir.path()));
	}

	QApplication app(argc, argv);
	retdec_gui_test_argc = argc;
	retdec_gui_test_argv = argv;

	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
