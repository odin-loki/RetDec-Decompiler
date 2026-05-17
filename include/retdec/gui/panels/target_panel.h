#ifndef RETDEC_GUI_PANELS_TARGET_PANEL_H
#define RETDEC_GUI_PANELS_TARGET_PANEL_H

#include "retdec/gui/panels/panel_base.h"

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
QT_END_NAMESPACE

namespace retdec {
namespace gui {
class ProjectFile;
namespace panels {

/**
 * @brief Editable arch / OS / entry for ProjectFile; binary path read-only.
 */
class TargetPanel : public PanelBase {
    Q_OBJECT
public:
    explicit TargetPanel(QWidget* parent = nullptr);

    void setFromProject(const ProjectFile* pf);
    void clear() override;

    /// Refresh decompiler --config path display from AppSettings (call after Settings dialog / menu Configure).
    void syncDecompilerConfigFromAppSettings();

    QString archText() const;
    QString osText() const;
    QString entryText() const;

signals:
    void applyRequested();

private slots:
    void onApplyClicked();
    void onBrowseDecompilerConfig();
    void onClearDecompilerConfig();
    void onUseBundledDecompilerConfig();
    void onEditDecompilerConfig();

private:
    void setupUi();
    void setFieldsEnabled(bool on);
    void updateDecompilerConfigButtons();

    QLabel* hint_ = nullptr;
    QLabel* binary_ = nullptr;
    QLineEdit* archEdit_ = nullptr;
    QLineEdit* osEdit_ = nullptr;
    QLineEdit* entryEdit_ = nullptr;
    QPushButton* applyBtn_ = nullptr;

    QLineEdit* decompilerConfigEdit_ = nullptr;
    QPushButton* browseCfgBtn_ = nullptr;
    QPushButton* bundledCfgBtn_ = nullptr;
    QPushButton* clearCfgBtn_ = nullptr;
    QPushButton* editCfgBtn_ = nullptr;
};

} // namespace panels
} // namespace gui
} // namespace retdec

#endif
