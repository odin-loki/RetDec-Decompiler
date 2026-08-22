/**
 * @file include/retdec/utils/debug.h
 * @brief Debug logging module.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_UTILS_DEBUG_H
#define RETDEC_UTILS_DEBUG_H

/// Loging msg print macro.
/// Define macro LOG_ENABLED to @c true before including this header to enable logging.
///
#define LOG \
	if (!LOG_ENABLED) {} \
	else std::cout

#endif
