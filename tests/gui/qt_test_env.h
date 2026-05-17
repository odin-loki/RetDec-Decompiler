/**
 * @file tests/gui/qt_test_env.h
 * @brief Shared argc/argv for GUI tests (see qt_test_main.cpp).
 *
 * retdec-gui-tests links every test TU into one executable whose main() constructs
 * exactly one QApplication. Do not create another QApplication in a test file
 * (static, SetUpTestSuite, etc.): it corrupts Qt and can yield
 * "QThread: Destroyed while thread is still running" or similar failures.
 *
 * In fixtures, use Q_ASSERT(QApplication::instance() != nullptr) in SetUp().
 * If a test uses Qt::QueuedConnection or QMetaObject::invokeMethod(..., QueuedConnection),
 * call QApplication::processEvents() before the receiver is destroyed (e.g. in TearDown
 * while widgets still exist).
 */
#pragma once

extern int retdec_gui_test_argc;
extern char** retdec_gui_test_argv;
