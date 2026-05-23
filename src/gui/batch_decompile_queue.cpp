/**
 * @file src/gui/batch_decompile_queue.cpp
 */

#include "retdec/gui/batch_decompile_queue.h"

#include <QFileInfo>

namespace retdec {
namespace gui {

QString batchDecompileStatusLabel(int index, int total, const QString& binaryPath) {
    const QString name = QFileInfo(binaryPath).fileName();
    return QStringLiteral("Batch %1/%2: %3").arg(index).arg(total).arg(name);
}

int batchDecompileCurrentIndex(int total, int remainingCount) {
    if (total <= 0 || remainingCount <= 0 || remainingCount > total)
        return 0;
    return total - remainingCount + 1;
}

void batchDecompilePopFront(QStringList* queue) {
    if (queue && !queue->isEmpty())
        queue->removeFirst();
}

} // namespace gui
} // namespace retdec
