/**
 * @file tests/gui/artifact_loader_test.cpp
 */

#include "retdec/gui/artifact_loader.h"

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <QFile>

TEST(ArtifactLoader, PathsFromOutputC) {
    const auto p = retdec::gui::pathsFromOutputC(
            QStringLiteral("C:/out/sample.gui-decompiled.c"));
    EXPECT_EQ(p.cPath, QStringLiteral("C:/out/sample.gui-decompiled.c"));
    EXPECT_EQ(p.configPath, QStringLiteral("C:/out/sample.gui-decompiled.config.json"));
    EXPECT_EQ(p.dsmPath, QStringLiteral("C:/out/sample.gui-decompiled.dsm"));
    EXPECT_EQ(p.llPath, QStringLiteral("C:/out/sample.gui-decompiled.ll"));
}

TEST(ArtifactLoader, LoadMinimalConfig) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString cPath = tmp.path() + QStringLiteral("/t.gui-decompiled.c");
    const QString cfgPath = tmp.path() + QStringLiteral("/t.gui-decompiled.config.json");

    QFile cFile(cPath);
    ASSERT_TRUE(cFile.open(QIODevice::WriteOnly));
    cFile.write("int main(void) { return 0; }\n");
    cFile.close();

    const char* cfg = R"({
        "functions": [{
            "name": "main",
            "demangledName": "main",
            "startAddr": "0x401000",
            "endAddr": "0x401010",
            "callingConvention": "cdecl",
            "fncType": "decompilerDefined",
            "basicBlocks": [{
                "startAddr": "0x401000",
                "endAddr": "0x401010",
                "preds": [],
                "succs": [],
                "calls": []
            }]
        }],
        "globals": []
    })";
    QFile cfgFile(cfgPath);
    ASSERT_TRUE(cfgFile.open(QIODevice::WriteOnly));
    cfgFile.write(cfg);
    cfgFile.close();

    retdec::gui::DecompileArtifacts art;
    const retdec::gui::DecompileArtifactPaths paths =
            retdec::gui::pathsFromOutputC(cPath);
    QString err;
    ASSERT_TRUE(retdec::gui::loadDecompileArtifactsFromPaths(paths, art, &err))
            << qPrintable(err);
    ASSERT_EQ(art.functions.size(), 1u);
    EXPECT_EQ(art.functions[0].name, QStringLiteral("main"));
    EXPECT_EQ(art.functions[0].address, 0x401000u);
    EXPECT_FALSE(art.cfgBlocks.empty());
}

TEST(ArtifactLoader, ExtractDsmSlice) {
    const QString dsm =
            QStringLiteral("; function: foo at 0x401000 -- 0x401010\n"
                           "0x401000:   90   nop\n"
                           "; function: bar at 0x402000 -- 0x402010\n");
    const QString slice = retdec::gui::extractDsmForFunction(
            dsm, 0x401000, 0x401010, QStringLiteral("foo"));
    EXPECT_TRUE(slice.contains(QStringLiteral("nop")));
    EXPECT_FALSE(slice.contains(QStringLiteral("bar")));
}
