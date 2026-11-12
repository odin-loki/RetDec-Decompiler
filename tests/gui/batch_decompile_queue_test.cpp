/**
 * @file tests/gui/batch_decompile_queue_test.cpp
 */

#include "retdec/gui/model/batch_decompile_queue.h"

#include <gtest/gtest.h>

namespace m = retdec::gui::model;

TEST(BatchDecompileQueue, StatusLabelUsesFileName)
{
	EXPECT_EQ(m::batchDecompileStatusLabel(2, 5, "C:/samples/foo.exe"), "Batch 2/5: foo.exe");
	EXPECT_EQ(m::batchDecompileStatusLabel(1, 1, "/home/u/bar"), "Batch 1/1: bar");
	EXPECT_EQ(m::batchDecompileStatusLabel(3, 4, "C:\\samples\\baz.dll"), "Batch 3/4: baz.dll");
}

TEST(BatchDecompileQueue, CurrentIndexFromRemaining)
{
	EXPECT_EQ(m::batchDecompileCurrentIndex(5, 5), 1);
	EXPECT_EQ(m::batchDecompileCurrentIndex(5, 3), 3);
	EXPECT_EQ(m::batchDecompileCurrentIndex(5, 1), 5);
	EXPECT_EQ(m::batchDecompileCurrentIndex(5, 0), 0);
	EXPECT_EQ(m::batchDecompileCurrentIndex(5, 6), 0);
	EXPECT_EQ(m::batchDecompileCurrentIndex(0, 0), 0);
}

TEST(BatchDecompileQueue, PopFront)
{
	std::vector<std::string> q{"a", "b"};
	m::batchDecompilePopFront(&q);
	ASSERT_EQ(q.size(), 1u);
	EXPECT_EQ(q.front(), "b");
	m::batchDecompilePopFront(&q);
	EXPECT_TRUE(q.empty());
	m::batchDecompilePopFront(&q);
	EXPECT_TRUE(q.empty());
	m::batchDecompilePopFront(static_cast<std::vector<std::string>*>(nullptr));
}

TEST(BatchDecompileQueue, PopFrontPaths)
{
	std::vector<std::filesystem::path> q{"/a/x.exe", "/b/y.exe"};
	m::batchDecompilePopFront(&q);
	ASSERT_EQ(q.size(), 1u);
	EXPECT_EQ(q.front(), std::filesystem::path("/b/y.exe"));
}
