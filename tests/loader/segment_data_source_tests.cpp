/**
 * @file tests/loader/segment_data_source_tests.cpp
 * @brief Tests for the @c segment_data_source module.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <gtest/gtest.h>

#include "retdec/loader/loader/segment_data_source.h"

using namespace ::testing;

namespace retdec {
namespace loader {
namespace tests {

#define EXPECT_ITERABLE_EQ(expected, actual, count) { \
	for (std::size_t i = 0; i < count; ++i) \
		EXPECT_EQ(expected[i], actual[i]); \
}

class SegmentDataSourceTests : public Test {};

TEST_F(SegmentDataSourceTests,
DefaultInitializationWorks) {
	SegmentDataSource dataSource;

	EXPECT_FALSE(dataSource.isDataSet());
}

TEST_F(SegmentDataSourceTests,
CustomInitializationWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	EXPECT_EQ(data.size(), dataSource.getDataSize());
	EXPECT_ITERABLE_EQ(data, dataSource.getData(), data.size());
}

TEST_F(SegmentDataSourceTests,
CopyInitializationWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);
	SegmentDataSource dataSourceCopy(dataSource);

	EXPECT_EQ(dataSource.getData(), dataSourceCopy.getData());
	EXPECT_EQ(dataSource.getDataSize(), dataSourceCopy.getDataSize());
}

TEST_F(SegmentDataSourceTests,
IsDataSetWorks) {
	std::vector<std::uint8_t> data  = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSetSource(dataRef);
	llvm::StringRef dataRef2 = llvm::StringRef(nullptr, 0);
	SegmentDataSource dataNotSetSource(dataRef2);

	EXPECT_TRUE(dataSetSource.isDataSet());
	EXPECT_FALSE(dataNotSetSource.isDataSet());
}

TEST_F(SegmentDataSourceTests,
GetDataWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	EXPECT_EQ(data.data(), dataSource.getData());
}

TEST_F(SegmentDataSourceTests,
GetDataSizeWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	EXPECT_EQ(data.size(), dataSource.getDataSize());
}

TEST_F(SegmentDataSourceTests,
ResizeToBiggerSizeForbiddenWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	dataSource.resize(10);

	EXPECT_EQ(data.size(), dataSource.getDataSize());
}

TEST_F(SegmentDataSourceTests,
ResizeToLowerSizeWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	dataSource.resize(2);

	EXPECT_EQ(2, dataSource.getDataSize());
}

TEST_F(SegmentDataSourceTests,
ShrinkChangingOnlySizeWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	EXPECT_TRUE(dataSource.shrink(0, 2));
	EXPECT_EQ(2, dataSource.getDataSize());
	EXPECT_ITERABLE_EQ(data, dataSource.getData(), dataSource.getDataSize());
}

TEST_F(SegmentDataSourceTests,
ShrinkChangingOnlyOffsetWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> expected(data.begin() + 2, data.end());

	EXPECT_TRUE(dataSource.shrink(2, data.size()));
	EXPECT_EQ(2, dataSource.getDataSize());
	EXPECT_ITERABLE_EQ(expected, dataSource.getData(), dataSource.getDataSize());
}

TEST_F(SegmentDataSourceTests,
ShrinkChangingOffsetAndSizeWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> expected(data.begin() + 2, data.end());

	EXPECT_TRUE(dataSource.shrink(2, 2));
	EXPECT_EQ(2, dataSource.getDataSize());
	EXPECT_ITERABLE_EQ(expected, dataSource.getData(), dataSource.getDataSize());
}

TEST_F(SegmentDataSourceTests,
ShrinkToBiggerSizeForbiddenWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	EXPECT_FALSE(dataSource.shrink(1, 5));
	EXPECT_EQ(data.size(), dataSource.getDataSize());
	EXPECT_ITERABLE_EQ(data, dataSource.getData(), dataSource.getDataSize());
}

TEST_F(SegmentDataSourceTests,
ShrinkToOffsetOutOfBoundsWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> expected = {};

	EXPECT_TRUE(dataSource.shrink(10, data.size()));
	EXPECT_EQ(0, dataSource.getDataSize());
	EXPECT_FALSE(dataSource.isDataSet());
}

TEST_F(SegmentDataSourceTests,
LoadDataWithUnsetDataWorks) {
	llvm::StringRef emptyRef = llvm::StringRef(nullptr, 0);
	SegmentDataSource dataSource(emptyRef);

	std::vector<std::uint8_t> result;
	EXPECT_FALSE(dataSource.isDataSet());
	EXPECT_FALSE(dataSource.loadData(0, 4, result));
}

TEST_F(SegmentDataSourceTests,
LoadDataFromOffsetOutOfBoundsWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> result;
	EXPECT_FALSE(dataSource.loadData(5, 1, result));
}

TEST_F(SegmentDataSourceTests,
LoadDataPartiallyExceedingSizeWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> expected = { 0x12, 0x13 };

	std::vector<std::uint8_t> result;
	EXPECT_TRUE(dataSource.loadData(2, 3, result));
	EXPECT_EQ(expected, result);
}

TEST_F(SegmentDataSourceTests,
LoadDataWithCorrectOffsetAndSizeWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> expected = { 0x11, 0x12 };

	std::vector<std::uint8_t> result;
	EXPECT_TRUE(dataSource.loadData(1, 2, result));
	EXPECT_EQ(expected, result);
}

TEST_F(SegmentDataSourceTests,
SaveDataWithUnsetDataWorks) {
	llvm::StringRef emptyRef = llvm::StringRef(nullptr, 0);
	SegmentDataSource dataSource(emptyRef);

	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12 };
	EXPECT_FALSE(dataSource.isDataSet());
	EXPECT_FALSE(dataSource.saveData(0, data.size(), data));
}

TEST_F(SegmentDataSourceTests,
SaveDataWithOffsetOutOfBoundsWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> value = { 0x20, 0x21, 0x22 };
	EXPECT_FALSE(dataSource.saveData(5, value.size(), value));
}

TEST_F(SegmentDataSourceTests,
SaveDataPartiallyExceedingSizeWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> result;
	std::vector<std::uint8_t> expected = { 0x10, 0x11, 0x20, 0x21 };

	std::vector<std::uint8_t> value = { 0x20, 0x21, 0x22 };
	EXPECT_TRUE(dataSource.saveData(2, value.size(), value));
	EXPECT_TRUE(dataSource.loadData(0, data.size(), result));
	EXPECT_EQ(data.size(), dataSource.getDataSize());
	EXPECT_EQ(expected, result);
}

TEST_F(SegmentDataSourceTests,
SaveDataWithCorrectOffsetAndSizeWorks) {
	std::vector<std::uint8_t> data = { 0x10, 0x11, 0x12, 0x13 };
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> result;
	std::vector<std::uint8_t> expected = { 0x10, 0x20, 0x21, 0x22 };

	std::vector<std::uint8_t> value = { 0x20, 0x21, 0x22 };
	EXPECT_TRUE(dataSource.saveData(1, value.size(), value));
	EXPECT_TRUE(dataSource.loadData(0, data.size(), result));
	EXPECT_EQ(data.size(), dataSource.getDataSize());
	EXPECT_EQ(expected, result);
}

//
// The clamps, against a size that wraps.
//
// `loadOffset + loadSize >= getDataSize()` and `saveOffset + saveSize >
// getDataSize()` were the containment tests, and both are sums of two values
// the caller supplies. At offset 1 with size 0xFFFFFFFFFFFFFFFF the sum is 0,
// which is not >= any data size, so the clamp was skipped and the size stayed
// at 18446744073709551615 -- read by the std::copy in each function, out of a
// four-byte buffer.
//
// These are not the clamp being made stricter. The four tests above this
// comment pin every in-range answer, and they pass unchanged.
//

TEST_F(SegmentDataSourceTests, LoadDataWithASizeThatWrapsIsClampedToWhatRemains)
{
	std::vector<std::uint8_t> data = {0x10, 0x11, 0x12, 0x13};
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> result;
	std::vector<std::uint8_t> expected = {0x11, 0x12, 0x13};

	EXPECT_TRUE(dataSource.loadData(1, 0xFFFFFFFFFFFFFFFFull, result));
	EXPECT_EQ(expected, result);
}

TEST_F(SegmentDataSourceTests, LoadDataWithASizeThatWrapsToASmallNumberIsClampedToo)
{
	std::vector<std::uint8_t> data = {0x10, 0x11, 0x12, 0x13};
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> result;
	std::vector<std::uint8_t> expected = {0x12, 0x13};

	// 2 + (2^64 - 1) is 1, which is smaller than the four bytes here.
	EXPECT_TRUE(dataSource.loadData(2, 0xFFFFFFFFFFFFFFFFull, result));
	EXPECT_EQ(expected, result);
}

TEST_F(SegmentDataSourceTests, SaveDataWithASizeThatWrapsIsClampedToWhatRemains)
{
	std::vector<std::uint8_t> data = {0x10, 0x11, 0x12, 0x13};
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> result;
	std::vector<std::uint8_t> expected = {0x10, 0x20, 0x21, 0x22};

	std::vector<std::uint8_t> value = {0x20, 0x21, 0x22};
	EXPECT_TRUE(dataSource.saveData(1, 0xFFFFFFFFFFFFFFFFull, value));
	EXPECT_TRUE(dataSource.loadData(0, data.size(), result));
	EXPECT_EQ(expected, result);
}

/// The bound the clamp never had. saveSize was checked against the destination
/// and never against the source, so this asked for a hundred bytes out of a
/// three-byte vector.
TEST_F(SegmentDataSourceTests, SaveDataDoesNotReadPastTheEndOfTheVectorItIsGiven)
{
	std::vector<std::uint8_t> data = {0x10, 0x11, 0x12, 0x13};
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> result;
	std::vector<std::uint8_t> expected = {0x20, 0x21, 0x22, 0x13};

	std::vector<std::uint8_t> value = {0x20, 0x21, 0x22};
	EXPECT_TRUE(dataSource.saveData(0, 100, value));
	EXPECT_TRUE(dataSource.loadData(0, data.size(), result));
	EXPECT_EQ(expected, result);
}

/// An offset at or past the end has nothing to read or write, whatever size
/// comes with it -- the guard the clamps rest on.
TEST_F(SegmentDataSourceTests, AnOffsetOutsideTheDataIsRefusedWhateverTheSize)
{
	std::vector<std::uint8_t> data = {0x10, 0x11, 0x12, 0x13};
	llvm::StringRef dataRef = llvm::StringRef(reinterpret_cast<const char*>(data.data()), data.size());
	SegmentDataSource dataSource(dataRef);

	std::vector<std::uint8_t> result;
	std::vector<std::uint8_t> value = {0x20};

	EXPECT_FALSE(dataSource.loadData(4, 1, result));
	EXPECT_FALSE(dataSource.loadData(0xFFFFFFFFFFFFFFFFull, 1, result));
	EXPECT_FALSE(dataSource.saveData(4, 1, value));
	EXPECT_FALSE(dataSource.saveData(0xFFFFFFFFFFFFFFFFull, 1, value));
}

} // namespace loader
} // namespace retdec
} // namespace tests
