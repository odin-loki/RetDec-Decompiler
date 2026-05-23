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
                           "0x401004:   c3   ret\n"
                           "; function: bar at 0x402000 -- 0x402010\n");
    const QString slice = retdec::gui::extractDsmForFunction(
            dsm, 0x401000, 0x401010, QStringLiteral("foo"));
    EXPECT_TRUE(slice.contains(QStringLiteral("nop")));
    EXPECT_FALSE(slice.contains(QStringLiteral("bar")));
}

TEST(ArtifactLoader, LoadTypeHierarchyAndCfgInstrs) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString cPath = tmp.path() + QStringLiteral("/t.gui-decompiled.c");
    const QString cfgPath = tmp.path() + QStringLiteral("/t.gui-decompiled.config.json");
    const QString dsmPath = tmp.path() + QStringLiteral("/t.gui-decompiled.dsm");

    QFile cFile(cPath);
    ASSERT_TRUE(cFile.open(QIODevice::WriteOnly));
    cFile.write("void fn(void) {}\n");
    cFile.close();

    const char* cfg = R"({
        "functions": [{
            "name": "fn",
            "startAddr": "0x401000",
            "endAddr": "0x401010",
            "basicBlocks": [{
                "startAddr": "0x401000",
                "endAddr": "0x401008",
                "preds": [],
                "succs": [],
                "calls": []
            }]
        }],
        "vtables": [{
            "name": "FnVtable",
            "address": "0x403000",
            "items": [{
                "address": "0x403000",
                "targetAddress": "0x401000",
                "targetName": "fn"
            }]
        }],
        "classes": [{
            "name": "Fn",
            "demangledName": "Fn",
            "superClasses": ["Base"],
            "virtualTables": ["FnVtable"],
            "methods": ["fn"]
        }],
        "globals": []
    })";
    QFile cfgFile(cfgPath);
    ASSERT_TRUE(cfgFile.open(QIODevice::WriteOnly));
    cfgFile.write(cfg);
    cfgFile.close();

    QFile dsmFile(dsmPath);
    ASSERT_TRUE(dsmFile.open(QIODevice::WriteOnly));
    dsmFile.write("0x401000:   55   push ebp\n0x401004:   5d   pop ebp\n");
    dsmFile.close();

    retdec::gui::DecompileArtifacts art;
    const retdec::gui::DecompileArtifactPaths paths =
            retdec::gui::pathsFromOutputC(cPath);
    QString err;
    ASSERT_TRUE(retdec::gui::loadDecompileArtifactsFromPaths(paths, art, &err))
            << qPrintable(err);

    ASSERT_EQ(art.typeHierarchyClasses.size(), 1);
    EXPECT_EQ(art.typeHierarchyClasses[0].name, QStringLiteral("Fn"));
    EXPECT_EQ(art.typeHierarchyClasses[0].vtableAddress, 0x403000u);
    ASSERT_EQ(art.typeHierarchyClasses[0].bases.size(), 1);
    EXPECT_EQ(art.typeHierarchyClasses[0].bases[0].base, QStringLiteral("Base"));
    ASSERT_EQ(art.typeHierarchyClasses[0].vtable.size(), 1);
    EXPECT_EQ(art.typeHierarchyClasses[0].vtable[0].funcName, QStringLiteral("fn"));

    const auto bbIt = art.cfgBlocks.find(0x401000u);
    ASSERT_NE(bbIt, art.cfgBlocks.end());
    ASSERT_EQ(bbIt->second.size(), 1u);
    EXPECT_GE(bbIt->second[0].instrs.size(), 1u);
    EXPECT_TRUE(bbIt->second[0].instrs[0].text.contains(QStringLiteral("push")));
}

TEST(ArtifactLoader, ParseStartEndLines) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString cPath = tmp.path() + QStringLiteral("/t.gui-decompiled.c");
    const QString cfgPath = tmp.path() + QStringLiteral("/t.gui-decompiled.config.json");

    QFile cFile(cPath);
    ASSERT_TRUE(cFile.open(QIODevice::WriteOnly));
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
    ASSERT_TRUE(cfgFile.open(QIODevice::WriteOnly));
    cfgFile.write(cfg);
    cfgFile.close();

    retdec::gui::DecompileArtifacts art;
    const retdec::gui::DecompileArtifactPaths paths =
            retdec::gui::pathsFromOutputC(cPath);
    QString err;
    ASSERT_TRUE(retdec::gui::loadDecompileArtifactsFromPaths(paths, art, &err));
    ASSERT_EQ(art.functions.size(), 1u);
    EXPECT_EQ(art.functions[0].startLine, 2);
    EXPECT_EQ(art.functions[0].endLine, 4);

    const QString slice = retdec::gui::extractCForFunction(
            cPath, art.functions[0].startLine, art.functions[0].endLine,
            QStringLiteral("foo"));
    EXPECT_TRUE(slice.contains(QStringLiteral("int foo")));
    EXPECT_TRUE(slice.contains(QStringLiteral("return 0")));
}
