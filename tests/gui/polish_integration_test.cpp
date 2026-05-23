/**
 * @file tests/gui/polish_integration_test.cpp
 * @brief Tier 6 polish integration tests: empty states, artifact line sync, layout smoke.
 */

#include "retdec/gui/artifact_loader.h"
#include "retdec/gui/widgets/empty_state_widget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QTemporaryDir>

namespace {

struct LineRangeFixture {
    QTemporaryDir tmp;
    QString cPath;

    bool create() {
        if (!tmp.isValid())
            return false;
        cPath = tmp.path() + QStringLiteral("/t.gui-decompiled.c");
        const QString cfgPath = tmp.path() + QStringLiteral("/t.gui-decompiled.config.json");

        QFile cFile(cPath);
        if (!cFile.open(QIODevice::WriteOnly))
            return false;
        cFile.write("line1\nint foo(void) {\n  return 0;\n}\n");
        cFile.close();

        const char* cfg = R"({
            "functions": [{
                "name": "foo",
                "startAddr": "0x401000",
                "endAddr": "0x401010",
                "startLine": "2",
                "endLine": "4"
            }],
            "globals": []
        })";
        QFile cfgFile(cfgPath);
        if (!cfgFile.open(QIODevice::WriteOnly))
            return false;
        cfgFile.write(cfg);
        cfgFile.close();
        return true;
    }
};

} // namespace

TEST(PolishIntegration, EmptyStateWidgetConstructs) {
    Q_ASSERT(QApplication::instance() != nullptr);
    retdec::gui::widgets::EmptyStateWidget widget;
    widget.setTitle(QStringLiteral("No content"));
    widget.setHint(QStringLiteral("Run decompilation or select a function."));
    widget.setIcon(QIcon());
    widget.show();
    QApplication::processEvents();
    EXPECT_TRUE(widget.isVisible());
}

TEST(PolishIntegration, ExtractCForFunctionMinimalConfig) {
    LineRangeFixture fx;
    ASSERT_TRUE(fx.create());

    retdec::gui::DecompileArtifacts art;
    const retdec::gui::DecompileArtifactPaths paths =
            retdec::gui::pathsFromOutputC(fx.cPath);
    QString err;
    ASSERT_TRUE(retdec::gui::loadDecompileArtifactsFromPaths(paths, art, &err))
            << qPrintable(err);
    ASSERT_EQ(art.functions.size(), 1u);

    const QString slice = retdec::gui::extractCForFunction(
            fx.cPath, art.functions[0].startLine, art.functions[0].endLine,
            QStringLiteral("foo"));
    EXPECT_TRUE(slice.contains(QStringLiteral("int foo")));
    EXPECT_TRUE(slice.contains(QStringLiteral("return 0")));
}

TEST(PolishIntegration, FunctionEntryHasStartEndLineFromArtifactLoader) {
    LineRangeFixture fx;
    ASSERT_TRUE(fx.create());

    retdec::gui::DecompileArtifacts art;
    const retdec::gui::DecompileArtifactPaths paths =
            retdec::gui::pathsFromOutputC(fx.cPath);
    QString err;
    ASSERT_TRUE(retdec::gui::loadDecompileArtifactsFromPaths(paths, art, &err))
            << qPrintable(err);
    ASSERT_EQ(art.functions.size(), 1u);
    EXPECT_EQ(art.functions[0].name, QStringLiteral("foo"));
    EXPECT_EQ(art.functions[0].startLine, 2);
    EXPECT_EQ(art.functions[0].endLine, 4);
}
