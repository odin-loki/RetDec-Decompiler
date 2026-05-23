/**
 * @file tests/gui/batch_decompile_queue_test.cpp
 */

#include "retdec/gui/batch_decompile_queue.h"

#include <gtest/gtest.h>

TEST(BatchDecompileQueue, StatusLabelUsesFileName) {
    const QString got = retdec::gui::batchDecompileStatusLabel(
            2, 5, QStringLiteral("C:/samples/foo.exe"));
    EXPECT_EQ(got, QStringLiteral("Batch 2/5: foo.exe"));
}

TEST(BatchDecompileQueue, CurrentIndexFromRemaining) {
    EXPECT_EQ(retdec::gui::batchDecompileCurrentIndex(5, 5), 1);
    EXPECT_EQ(retdec::gui::batchDecompileCurrentIndex(5, 3), 3);
    EXPECT_EQ(retdec::gui::batchDecompileCurrentIndex(5, 1), 5);
    EXPECT_EQ(retdec::gui::batchDecompileCurrentIndex(5, 0), 0);
}

TEST(BatchDecompileQueue, PopFront) {
    QStringList q{QStringLiteral("a"), QStringLiteral("b")};
    retdec::gui::batchDecompilePopFront(&q);
    ASSERT_EQ(q.size(), 1);
    EXPECT_EQ(q.front(), QStringLiteral("b"));
    retdec::gui::batchDecompilePopFront(&q);
    EXPECT_TRUE(q.isEmpty());
}
