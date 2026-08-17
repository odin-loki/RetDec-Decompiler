/**
 * @file tests/gui/export_bundle_test.cpp
 */

#include "retdec/gui/export_bundle.h"
#include "retdec/gui/zip_writer.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

bool zipContainsEntryName(const QByteArray& zipBytes, const QByteArray& name)
{
	return zipBytes.contains(name);
}

} // namespace

TEST(ZipWriter, RoundTripStoreEntry)
{
	QTemporaryDir tmp;
	ASSERT_TRUE(tmp.isValid());
	const QString zipPath = tmp.path() + QStringLiteral("/test.zip");

	{
		retdec::gui::ZipWriter zip(zipPath);
		ASSERT_TRUE(zip.isOpen());
		ASSERT_TRUE(zip.addEntry(QStringLiteral("hello.txt"), QByteArray("hello bundle")));
		ASSERT_TRUE(zip.close());
	}

	QFile f(zipPath);
	ASSERT_TRUE(f.open(QIODevice::ReadOnly));
	const QByteArray bytes = f.readAll();
	EXPECT_GE(bytes.size(), 22);
	EXPECT_EQ(bytes.left(4), QByteArray("PK\x03\x04", 4));
	EXPECT_TRUE(zipContainsEntryName(bytes, "hello.txt"));
}

TEST(ExportBundle, IncludesArtifactsAndCommand)
{
	QTemporaryDir tmp;
	ASSERT_TRUE(tmp.isValid());
	const QString cPath = tmp.path() + QStringLiteral("/sample.gui-decompiled.c");
	const QString cfgPath = tmp.path() + QStringLiteral("/sample.gui-decompiled.config.json");
	const QString zipPath = tmp.path() + QStringLiteral("/out.zip");

	QFile cFile(cPath);
	ASSERT_TRUE(cFile.open(QIODevice::WriteOnly));
	cFile.write("int main(void) { return 0; }\n");
	cFile.close();

	QFile cfgFile(cfgPath);
	ASSERT_TRUE(cfgFile.open(QIODevice::WriteOnly));
	cfgFile.write("{}");
	cfgFile.close();

	retdec::gui::DecompileBundleInput in;
	in.cPath = cPath;
	in.decompilerExe = QStringLiteral("C:/tools/retdec-decompiler.exe");
	in.decompilerCwd = QStringLiteral("C:/tools");
	in.decompilerArgs = {QStringLiteral("--input-file"), QStringLiteral("sample.exe")};
	in.logInline = QByteArray("stage: done\n");

	QString err;
	ASSERT_TRUE(retdec::gui::exportDecompileBundle(in, zipPath, &err)) << qPrintable(err);
	ASSERT_TRUE(QFileInfo::exists(zipPath));

	QFile zipFile(zipPath);
	ASSERT_TRUE(zipFile.open(QIODevice::ReadOnly));
	const QByteArray bytes = zipFile.readAll();
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.c"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.config.json"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "decompiler-command.txt"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "decompiler.log"));
	EXPECT_TRUE(bytes.contains("--input-file"));
}

TEST(ExportBundle, ExportBundleIncludesOptionalSidecars)
{
	QTemporaryDir tmp;
	ASSERT_TRUE(tmp.isValid());
	const QString cPath = tmp.path() + QStringLiteral("/sample.gui-decompiled.c");
	const QString cfgPath = tmp.path() + QStringLiteral("/sample.gui-decompiled.config.json");
	const QString zipPath = tmp.path() + QStringLiteral("/out.zip");

	QFile cFile(cPath);
	ASSERT_TRUE(cFile.open(QIODevice::WriteOnly));
	cFile.write("int main(void) { return 0; }\n");
	cFile.close();

	QFile cfgFile(cfgPath);
	ASSERT_TRUE(cfgFile.open(QIODevice::WriteOnly));
	cfgFile.write("{}");
	cfgFile.close();

	QFile refinedFile(cPath + QStringLiteral(".refined.c"));
	ASSERT_TRUE(refinedFile.open(QIODevice::WriteOnly));
	refinedFile.write("int refined(void) { return 1; }\n");
	refinedFile.close();

	QFile manifestFile(cPath + QStringLiteral(".refinement-manifest.json"));
	ASSERT_TRUE(manifestFile.open(QIODevice::WriteOnly));
	manifestFile.write("{\"accepted\":true}");
	manifestFile.close();

	QFile typeFile(cPath + QStringLiteral(".type-inference.json"));
	ASSERT_TRUE(typeFile.open(QIODevice::WriteOnly));
	typeFile.write("{\"ok\":true}");
	typeFile.close();

	retdec::gui::DecompileBundleInput in;
	in.cPath = cPath;
	in.decompilerExe = QStringLiteral("C:/tools/retdec-decompiler.exe");
	in.decompilerCwd = QStringLiteral("C:/tools");
	in.decompilerArgs = {QStringLiteral("--input-file"), QStringLiteral("sample.exe")};
	in.logInline = QByteArray("stage: done\n");

	QString err;
	ASSERT_TRUE(retdec::gui::exportDecompileBundle(in, zipPath, &err)) << qPrintable(err);
	ASSERT_TRUE(QFileInfo::exists(zipPath));

	QFile zipFile(zipPath);
	ASSERT_TRUE(zipFile.open(QIODevice::ReadOnly));
	const QByteArray bytes = zipFile.readAll();
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.c.refined.c"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.c.refinement-manifest.json"));
	EXPECT_TRUE(zipContainsEntryName(bytes, "sample.gui-decompiled.c.type-inference.json"));
}

TEST(ExportBundle, MissingCFileFails)
{
	retdec::gui::DecompileBundleInput in;
	in.cPath = QStringLiteral("C:/missing/sample.gui-decompiled.c");
	QString err;
	EXPECT_FALSE(retdec::gui::exportDecompileBundle(in, QStringLiteral("C:/out.zip"), &err));
	EXPECT_FALSE(err.isEmpty());
}
