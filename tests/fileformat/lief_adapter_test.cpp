#include <gtest/gtest.h>

#include "retdec/fileformat/lief_adapter.h"

TEST(LiefAdapterTest, AvailableMatchesBuild)
{
#if defined(RETDEC_HAS_LIEF)
	EXPECT_TRUE(retdec::fileformat::LiefAdapter::available());
#else
	EXPECT_FALSE(retdec::fileformat::LiefAdapter::available());
#endif
}

TEST(LiefAdapterTest, ParseSectionsEmptyWhenUnavailable)
{
#if !defined(RETDEC_HAS_LIEF)
	EXPECT_TRUE(retdec::fileformat::LiefAdapter::parseSections("/nonexistent").empty());
#endif
}
