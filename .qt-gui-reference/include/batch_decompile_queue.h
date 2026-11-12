/**
 * @file include/retdec/gui/batch_decompile_queue.h
 * @brief Helpers for sequential batch decompile queue state.
 */

#ifndef RETDEC_GUI_BATCH_DECOMPILE_QUEUE_H
#define RETDEC_GUI_BATCH_DECOMPILE_QUEUE_H

#include <QString>
#include <QStringList>

namespace retdec {
namespace gui {

/// Status-bar label: "Batch 2/5: filename.exe"
QString batchDecompileStatusLabel(int index, int total, const QString& binaryPath);

/// Current item index (1-based) while @p queue holds remaining paths (front = active).
int batchDecompileCurrentIndex(int total, int remainingCount);

/// Remove the front item after a run finishes or is skipped.
void batchDecompilePopFront(QStringList* queue);

} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_BATCH_DECOMPILE_QUEUE_H
