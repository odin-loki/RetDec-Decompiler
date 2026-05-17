/**
 * @file include/retdec/qwen3/qwen3_trace.h
 * @brief Optional stderr tracing for Qwen3 inference (crash narrowing).
 *
 * Set environment variable `RETDEC_QWEN3_TRACE` to a truthy value (`1`, `true`,
 * `yes`, case-insensitive) before starting the process. Each log line is flushed
 * immediately so the last printed line is usually the stage right before a hard
 * crash. Optional `RETDEC_QWEN3_TRACE_VERBOSE=1` adds finer MoE expert logs.
 */

#ifndef RETDEC_QWEN3_TRACE_H
#define RETDEC_QWEN3_TRACE_H

namespace retdec::qwen3 {

bool traceEnabled();
bool traceVerbose();

void tracef(const char* fmt, ...);

} // namespace retdec::qwen3

#endif
