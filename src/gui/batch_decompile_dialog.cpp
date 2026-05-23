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
    box->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Start"));
    lay->addWidget(box);

    connect(addBtn, &QPushButton::clicked, this, &BatchDecompileDialog::onAdd);
    connect(removeBtn, &QPushButton::clicked, this, &BatchDecompileDialog::onRemove);
    connect(clearBtn, &QPushButton::clicked, this, &BatchDecompileDialog::onClear);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QStringList BatchDecompileDialog::binaryPaths() const {
    QStringList out;
    for (int i = 0; i < list_->count(); ++i)
        out << list_->item(i)->text();
    return out;
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
}

void BatchDecompileDialog::onRemove() {
    qDeleteAll(list_->selectedItems());
}

void BatchDecompileDialog::onClear() {
    list_->clear();
}

} // namespace gui
} // namespace retdec
