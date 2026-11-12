// AccessKit backend for fuse_ui_a11y (built when FUSE_UI_ACCESSKIT_DIR points at an accesskit-c build).
#include "fuse/ui/a11y/a11y.hpp"

#if FUSE_UI_HAVE_ACCESSKIT
#include "accesskit.h"

namespace fuse::ui::a11y {

bool accessKitAvailable() { return true; }

namespace {
accesskit_role roleOf(A11yRole r) {
    switch (r) {
    case A11yRole::Window: return ACCESSKIT_ROLE_WINDOW;
    case A11yRole::Group: return ACCESSKIT_ROLE_GROUP;
    case A11yRole::Text: return ACCESSKIT_ROLE_LABEL;
    case A11yRole::Button: return ACCESSKIT_ROLE_BUTTON;
    case A11yRole::CheckBox: return ACCESSKIT_ROLE_CHECK_BOX;
    case A11yRole::RadioButton: return ACCESSKIT_ROLE_RADIO_BUTTON;
    case A11yRole::Switch: return ACCESSKIT_ROLE_SWITCH;
    case A11yRole::Slider: case A11yRole::Dial: return ACCESSKIT_ROLE_SLIDER;
    case A11yRole::ProgressBar: return ACCESSKIT_ROLE_PROGRESS_INDICATOR;
    case A11yRole::TextField: return ACCESSKIT_ROLE_TEXT_INPUT;
    case A11yRole::ScrollArea: return ACCESSKIT_ROLE_SCROLL_VIEW;
    case A11yRole::List: return ACCESSKIT_ROLE_LIST;
    case A11yRole::ListItem: return ACCESSKIT_ROLE_LIST_ITEM;
    case A11yRole::Image: return ACCESSKIT_ROLE_IMAGE;
    case A11yRole::Separator: return ACCESSKIT_ROLE_SPLITTER;
    case A11yRole::Link: return ACCESSKIT_ROLE_LINK;
    case A11yRole::ComboBox: return ACCESSKIT_ROLE_COMBO_BOX;
    case A11yRole::SpinBox: return ACCESSKIT_ROLE_SPIN_BUTTON;
    case A11yRole::ScrollBar: return ACCESSKIT_ROLE_SCROLL_BAR;
    case A11yRole::Tab: return ACCESSKIT_ROLE_TAB;
    case A11yRole::None: break;
    }
    return ACCESSKIT_ROLE_UNKNOWN;
}
bool ranged(A11yRole r) {
    return r == A11yRole::Slider || r == A11yRole::Dial || r == A11yRole::ProgressBar || r == A11yRole::SpinBox || r == A11yRole::ScrollBar;
}
bool toggles(A11yRole r) { return r == A11yRole::CheckBox || r == A11yRole::RadioButton || r == A11yRole::Switch; }
} // namespace

void* buildAccessKitUpdate(const Bridge& b, bool includeTree) {
    const auto& v = b.nodes();
    accesskit_tree_update* u = accesskit_tree_update_with_capacity_and_focus(v.size(), b.focusId());
    std::vector<accesskit_node_id> kids;
    for (u32 i = 0; i < v.size(); ++i) {
        const A11ySnapshotNode& s = v[i];
        accesskit_node* n = accesskit_node_new(roleOf(s.role));
        if (!s.name.empty()) accesskit_node_set_label_with_length(n, s.name.data(), s.name.size());
        accesskit_node_set_bounds(n, {double(s.bounds.x), double(s.bounds.y), double(s.bounds.x + s.bounds.w), double(s.bounds.y + s.bounds.h)});
        if (s.disabled) accesskit_node_set_disabled(n);
        if (s.focusable) accesskit_node_add_action(n, ACCESSKIT_ACTION_FOCUS);
        if (s.focusable && !ranged(s.role) && s.role != A11yRole::TextField) accesskit_node_add_action(n, ACCESSKIT_ACTION_CLICK);
        if (toggles(s.role)) accesskit_node_set_toggled(n, s.checked ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
        if (ranged(s.role)) {
            accesskit_node_set_numeric_value(n, double(s.value));
            accesskit_node_set_min_numeric_value(n, double(s.lo));
            accesskit_node_set_max_numeric_value(n, double(s.hi));
            if (s.role != A11yRole::ProgressBar) {
                accesskit_node_add_action(n, ACCESSKIT_ACTION_INCREMENT);
                accesskit_node_add_action(n, ACCESSKIT_ACTION_DECREMENT);
                accesskit_node_add_action(n, ACCESSKIT_ACTION_SET_VALUE);
            }
        }
        kids.clear();
        for (u32 c : b.children(i)) kids.push_back(idOf(v[c].node));
        if (!kids.empty()) accesskit_node_set_children(n, kids.size(), kids.data());
        accesskit_tree_update_push_node(u, idOf(s.node), n); // takes ownership
    }
    if (includeTree && !v.empty()) {
        accesskit_tree* t = accesskit_tree_new(idOf(v[0].node));
        accesskit_tree_set_toolkit_name(t, "FUSE UI");
        accesskit_tree_set_toolkit_version(t, "0.1");
        accesskit_tree_update_set_tree(u, t);
    }
    return u;
}

std::string accessKitDebug(const void* update) {
    char* d = accesskit_tree_update_debug(static_cast<const accesskit_tree_update*>(update));
    std::string s = d ? d : "";
    accesskit_string_free(d);
    return s;
}
void freeAccessKitUpdate(void* update) { accesskit_tree_update_free(static_cast<accesskit_tree_update*>(update)); }

#if defined(__linux__)
namespace {
accesskit_tree_update* activate(void* user) {
    Bridge* b = static_cast<Bridge*>(user);
    // called on AccessKit's thread: build from the last snapshot (the UI thread refreshes it in update())
    auto lock = b->lockSnapshot();
    return static_cast<accesskit_tree_update*>(buildAccessKitUpdate(*b, true));
}
void onAction(accesskit_action_request* req, void* user) {
    Bridge* b = static_cast<Bridge*>(user);
    ActionRequest r;
    r.target = req->target_node;
    switch (req->action) {
    case ACCESSKIT_ACTION_FOCUS: r.action = Action::Focus; break;
    case ACCESSKIT_ACTION_CLICK: r.action = Action::Click; break;
    case ACCESSKIT_ACTION_INCREMENT: r.action = Action::Increment; break;
    case ACCESSKIT_ACTION_DECREMENT: r.action = Action::Decrement; break;
    case ACCESSKIT_ACTION_SET_VALUE:
        r.action = Action::SetValue;
        if (req->data.has_value && req->data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE) r.value = f32(req->data.value.numeric_value);
        break;
    default: accesskit_action_request_free(req); return;
    }
    accesskit_action_request_free(req);
    b->enqueue(r);
}
void onDeactivate(void*) {}
accesskit_tree_update* factory(void* user) { return static_cast<accesskit_tree_update*>(buildAccessKitUpdate(*static_cast<Bridge*>(user), false)); }
} // namespace

UnixAdapter::UnixAdapter(Bridge& b) : m_bridge(b) {
    m_bridge.sync();
    m_adapter = accesskit_unix_adapter_new(activate, &m_bridge, onAction, &m_bridge, onDeactivate, &m_bridge);
}
UnixAdapter::~UnixAdapter() {
    if (m_adapter) accesskit_unix_adapter_free(static_cast<accesskit_unix_adapter*>(m_adapter));
}
void UnixAdapter::update() {
    m_bridge.drain();
    if (m_bridge.sync() && m_adapter)
        accesskit_unix_adapter_update_if_active(static_cast<accesskit_unix_adapter*>(m_adapter), factory, &m_bridge);
}
void UnixAdapter::setWindowFocused(bool focused) {
    if (m_adapter) accesskit_unix_adapter_update_window_focus_state(static_cast<accesskit_unix_adapter*>(m_adapter), focused);
}
#endif

} // namespace fuse::ui::a11y

#else
namespace fuse::ui::a11y {
bool accessKitAvailable() { return false; }
} // namespace fuse::ui::a11y
#endif
