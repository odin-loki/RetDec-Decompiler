/**
 * @file src/gui/batch_decompile_dialog.cpp
 */

#include "retdec/gui/batch_decompile_dialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace retdec {
namespace gui {

BatchDecompileDialog::BatchDecompileDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Batch Decompile"));
    resize(560, 360);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(
            QStringLiteral("Binaries are decompiled sequentially using the same "
                           "settings as Run Full Analysis.")));

    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    lay->addWidget(list_, 1);

    hintLabel_ = new QLabel(
            QStringLiteral("Add at least one binary to enable Start."), this);
    hintLabel_->setProperty("role", "muted");
    lay->addWidget(hintLabel_);

    auto* btnRow = new QHBoxLayout();
    auto* addBtn = new QPushButton(QStringLiteral("Add…"), this);
    auto* removeBtn = new QPushButton(QStringLiteral("Remove"), this);
    auto* clearBtn = new QPushButton(QStringLiteral("Clear"), this);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(removeBtn);
    btnRow->addWidget(clearBtn);
    btnRow->addStretch();
    lay->addLayout(btnRow);

    auto* box = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    startBtn_ = box->button(QDialogButtonBox::Ok);
    startBtn_->setText(QStringLiteral("Start"));
    lay->addWidget(box);

    connect(addBtn, &QPushButton::clicked, this, &BatchDecompileDialog::onAdd);
    connect(removeBtn, &QPushButton::clicked, this, &BatchDecompileDialog::onRemove);
    connect(clearBtn, &QPushButton::clicked, this, &BatchDecompileDialog::onClear);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateStartEnabled();
}

QStringList BatchDecompileDialog::binaryPaths() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i)
        out << list_->item(i)->text();
    return out;
}

void BatchDecompileDialog::updateStartEnabled() {
    const bool hasItems = list_->count() > 0;
    if (startBtn_)
        startBtn_->setEnabled(hasItems);
    if (hintLabel_)
        hintLabel_->setVisible(!hasItems);
}

void BatchDecompileDialog::onAdd() {
    const QStringList paths = QFileDialog::getOpenFileNames(
            this, QStringLiteral("Add binaries"), QString(),
            QStringLiteral("Executable Files (*.exe *.dll *.elf *.so *.dylib *.bin);;"
                           "All Files (*)"));
    for (const QString& p : paths) {
        const QString abs = QFileInfo(p).absoluteFilePath();
        bool dup = false;
        for (int i = 0; i < list_->count(); ++i) {
            if (list_->item(i)->text() == abs) {
                dup = true;
                break;
            }
        }
        if (!dup)
            list_->addItem(abs);
    }
    updateStartEnabled();
}

void BatchDecompileDialog::onRemove() {
    qDeleteAll(list_->selectedItems());
    updateStartEnabled();
}

void BatchDecompileDialog::onClear() {
    list_->clear();
    updateStartEnabled();
}

} // namespace gui
} // namespace retdec
