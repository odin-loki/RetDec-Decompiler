/**
 * @file include/retdec/gui/model/batch_decompile_queue.h
 * @brief Helpers for the sequential batch-decompile queue.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_BATCH_DECOMPILE_QUEUE_H
#define RETDEC_GUI_MODEL_BATCH_DECOMPILE_QUEUE_H

#include <filesystem>
#include <string>
#include <vector>

namespace retdec::gui::model {

/// Status-bar label: "Batch 2/5: filename.exe" (file name only).
std::string batchDecompileStatusLabel(int index, int total, const std::filesystem::path& binaryPath);

/// Current item index (1-based) while the queue holds @p remainingCount paths
/// (front = active). 0 when the counts make no sense.
int batchDecompileCurrentIndex(int total, int remainingCount);

/// Remove the front item after a run finishes or is skipped (no-op on null/empty).
void batchDecompilePopFront(std::vector<std::string>* queue);
void batchDecompilePopFront(std::vector<std::filesystem::path>* queue);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_BATCH_DECOMPILE_QUEUE_H
