#include "retdec/gui/panels/panel_base.h"

namespace retdec::gui::panels {

PanelBase::PanelBase(const QString& title, QWidget* parent)
    : QWidget(parent), title_(title) {}

} // namespace retdec::gui::panels
