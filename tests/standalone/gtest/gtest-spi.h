/**
 * @file tests/standalone/gtest/gtest-spi.h
 * @brief GoogleTest's self-test header, for the dependency-free shim.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Real GoogleTest splits EXPECT_NONFATAL_FAILURE and friends into
 * <gtest/gtest-spi.h> so that ordinary tests do not see them. The shim defines
 * them in gtest.h and this file exists so a test that needs them can write the
 * same include either way:
 *
 *     #include <gtest/gtest.h>
 *     #include <gtest/gtest-spi.h>
 *
 * and build both against GoogleTest under CMake and against the shim under
 * scripts/standalone_check.sh.
 */

#ifndef RETDEC_TESTS_STANDALONE_GTEST_SPI_H
#define RETDEC_TESTS_STANDALONE_GTEST_SPI_H

#include "gtest.h"

#endif // RETDEC_TESTS_STANDALONE_GTEST_SPI_H
