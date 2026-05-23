/**
 * @file include/retdec/gui/batch_decompile_dialog.h
 * @brief Dialog to collect binary paths for batch decompile.
 */

#ifndef RETDEC_GUI_BATCH_DECOMPILE_DIALOG_H
#define RETDEC_GUI_BATCH_DECOMPILE_DIALOG_H

#include <QDialog>
#include <QStringList>

class QListWidget;

namespace retdec {
namespace gui {

class BatchDecompileDialog : public QDialog {
    Q_OBJECT

public:
    explicit BatchDecompileDialog(QWidget* parent = nullptr);

    QStringList binaryPaths() const;

private:
    void onAdd();
    void onRemove();
    void onClear();

    QListWidget* list_ = nullptr;
};

} // namespace gui
} // namespace retdec

#endif // RETDEC_GUI_BATCH_DECOMPILE_DIALOG_H
