#include "fuse/ui/immediate.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::ui::immediate {

Metrics Metrics::fromFont(const fuse::text::Font& f, float px) {
    std::vector<fuse::text::ShapedGlyph> s;
    const float w = fuse::text::measureText(f, "0", px, s);
    const auto m = f.metrics(px);
    return {std::int32_t(std::ceil(w)), std::int32_t(std::ceil(m.ascent - m.descent)), std::int32_t(std::ceil(m.lineHeight())) + 3};
}

ImmediateUi::ImmediateUi(Metrics m) : m_metrics(m) {
    m_draws.reserve(1024);
    m_widgets.reserve(512);
    m_lastWidgets.reserve(512);
    m_shape.reserve(256);
}

void ImmediateUi::setFont(const fuse::text::Font* font, fuse::text::GlyphAtlas* atlas, float px) {
    m_font = font;
    m_atlas = atlas;
    m_px = px;
}

void ImmediateUi::begin(const IRect& panel, std::int32_t scale, const InputEvent* event) {
    m_panel = panel;
    m_scale = scale < 1 ? 1 : scale;
    m_pad = 2 * m_scale;
    m_cursorX = m_panel.x + m_pad;
    m_cursorY = m_panel.y + m_pad;
    m_lineY = m_cursorY;
    m_sameLine = m_lineOpen = m_keyConsumed = false;
    m_runs.clear();
    m_runText.clear();
    m_event = event ? *event : InputEvent{};
    switch (m_event.type) {
    case EventType::MouseMove:
    case EventType::MouseUp:
        m_mouseX = m_event.x;
        m_mouseY = m_event.y;
        break;
    case EventType::MouseDown:
        m_mouseX = m_event.x;
        m_mouseY = m_event.y;
        m_focus = 0;
        break;
    default: break;
    }
    m_draws.clear();
    m_widgets.clear();
    m_draws.rect({f32(panel.x), f32(panel.y), f32(panel.w), f32(panel.h)}, m_theme.panel, m_theme.radius * f32(m_scale));
}

void ImmediateUi::end() {
    if (m_event.type == EventType::MouseUp) m_active = 0;
    m_lastWidgets.swap(m_widgets);
}

bool ImmediateUi::place(std::int32_t widthPx, IRect& out) {
    const std::int32_t lineH = m_metrics.lineHeight * m_scale;
    const std::int32_t right = m_panel.x + m_panel.w - m_pad, bottom = m_panel.y + m_panel.h - m_pad;
    std::int32_t x = m_panel.x + m_pad;
    std::int32_t y = m_lineOpen ? m_lineY + lineH : m_panel.y + m_pad;
    if (m_sameLine && m_lineOpen) {
        x = m_cursorX + m_scale * 2;
        y = m_lineY;
    }
    m_sameLine = false;
    const std::int32_t h = (m_metrics.glyphHeight + 2) * m_scale;
    if (y + h > bottom) {
        m_lineY = y;
        m_lineOpen = true;
        m_cursorX = x;
        return false;
    }
    if (x >= right) return false;
    out = IRect{x, y, std::min(widthPx, right - x), h};
    m_lineY = y;
    m_lineOpen = true;
    m_cursorX = x + out.w;
    return !out.empty();
}

std::int32_t ImmediateUi::linesLeft() const {
    const std::int32_t lineH = m_metrics.lineHeight * m_scale, bottom = m_panel.y + m_panel.h - m_pad;
    const std::int32_t next = m_lineOpen ? m_lineY + lineH : m_panel.y + m_pad;
    const std::int32_t h = (m_metrics.glyphHeight + 2) * m_scale;
    return next + h > bottom ? 0 : 1 + (bottom - next - h) / lineH;
}

std::int32_t ImmediateUi::charsLeft() const {
    const std::int32_t right = m_panel.x + m_panel.w - m_pad;
    const std::int32_t x = (m_sameLine && m_lineOpen) ? m_cursorX + m_scale * 2 : m_panel.x + m_pad;
    return std::max<std::int32_t>(0, (right - x) / (m_metrics.cellWidth * m_scale));
}

void ImmediateUi::spacing(std::int32_t lines) {
    for (std::int32_t i = 0; i < lines; ++i) {
        IRect r;
        place(1, r);
    }
}

void ImmediateUi::fill(const IRect& r, Color c) {
    if (!r.empty() && c.a > 0.f) m_draws.rect({f32(r.x), f32(r.y), f32(r.w), f32(r.h)}, c, m_theme.radius * f32(m_scale));
}

void ImmediateUi::glyphs(std::int32_t x, std::int32_t y, std::string_view s, Color c, std::int32_t clipW) {
    TextRun run;
    run.x = x;
    run.y = y;
    run.clipW = clipW;
    run.color = c;
    run.textBegin = std::uint32_t(m_runText.size());
    run.textLength = std::uint32_t(s.size());
    run.firstCmd = std::uint32_t(m_draws.size());
    m_runText.insert(m_runText.end(), s.begin(), s.end());
    struct Close {
        ImmediateUi& ui;
        TextRun& r;
        ~Close() {
            r.cmdCount = std::uint32_t(ui.m_draws.size()) - r.firstCmd;
            ui.m_runs.push_back(r);
        }
    } close{*this, run};
    if (!m_font || !m_atlas || s.empty() || clipW <= 0) return;
    const f32 px = m_px * f32(m_scale);
    fuse::text::shapeText(*m_font, s, px, m_shape);
    const auto fm = m_font->metrics(px);
    const f32 boxH = f32((m_metrics.glyphHeight + 2) * m_scale);
    const f32 base = std::round(f32(y) - f32(m_scale) + (boxH - (fm.ascent - fm.descent)) * 0.5f + fm.ascent);
    for (const auto& g : m_shape) {
        if (g.x + g.advance > f32(clipW) + 0.5f) break;
        if (g.glyph == fuse::text::kInvisibleGlyph) continue;
        fuse::text::forEachPaintGlyph(*m_font, g, px, [&](const fuse::text::Font& f, std::uint32_t glyph, f32 fpx,
                                                          const fuse::text::ColorLayer* layer) {
            const auto ag = m_atlas->get(f, glyph, fpx);
            if (!ag.valid || ag.w == 0) return;
            DrawCmd d;
            d.op = DrawOp::Glyph;
            d.rect = {std::round(f32(x) + g.x) + f32(ag.x0), base + f32(ag.y0), f32(ag.w), f32(ag.h)};
            d.u = ag.u; d.v = ag.v; d.tw = ag.w; d.th = ag.h;
            d.fill = (!layer || layer->foreground)
                         ? c
                         : Color{f32(layer->r) / 255.f, f32(layer->g) / 255.f, f32(layer->b) / 255.f, f32(layer->a) / 255.f * c.a};
            m_draws.push(d);
        });
    }
}

void ImmediateUi::separator() {
    IRect r;
    if (place(m_panel.w, r)) m_draws.rect({f32(r.x), f32(r.y + r.h / 2), f32(r.w), f32(m_scale)}, m_theme.dim);
}

void ImmediateUi::text(std::string_view s) { text(s, m_theme.text); }
void ImmediateUi::text(std::string_view s, Color c) {
    IRect r;
    if (place(std::int32_t(s.size()) * m_metrics.cellWidth * m_scale, r)) glyphs(r.x, r.y + m_scale, s, c, r.w);
}

void ImmediateUi::title(std::string_view s) {
    IRect r;
    if (place(m_panel.w, r)) {
        fill(r, m_theme.title);
        glyphs(r.x + 2 * m_scale, r.y + m_scale, s, m_theme.text, r.w - 2 * m_scale);
    }
}

bool ImmediateUi::interact(std::uint32_t id, const IRect& r, bool& hot) {
    hot = r.contains(m_mouseX, m_mouseY);
    bool clicked = false;
    if (m_event.type == EventType::MouseDown && hot) m_active = id;
    else if (m_event.type == EventType::MouseUp && m_active == id && hot) clicked = true;
    m_widgets.push_back({id, r});
    return clicked;
}

bool ImmediateUi::button(std::string_view idText, std::string_view label, bool selected) {
    IRect r;
    if (!place((std::int32_t(label.size()) * m_metrics.cellWidth + 3) * m_scale, r)) return false;
    const u32 id = hashId(idText);
    bool hot = false;
    const bool clicked = interact(id, r, hot);
    fill(r, selected ? m_theme.buttonOn : (hot ? m_theme.buttonHot : m_theme.button));
    glyphs(r.x + 2 * m_scale, r.y + m_scale, label, m_theme.text, r.w - 2 * m_scale);
    return clicked;
}

bool ImmediateUi::textField(std::string_view idText, std::string& value, std::int32_t widthChars, std::size_t maxLength) {
    IRect r;
    if (!place((widthChars * m_metrics.cellWidth + 3) * m_scale, r)) return false;
    const u32 id = hashId(idText);
    bool hot = false;
    interact(id, r, hot);
    if (m_event.type == EventType::MouseDown && hot) m_focus = id;
    bool changed = false;
    if (m_focus == id) {
        if (m_event.type == EventType::Char && m_event.value >= 32 && m_event.value <= 126 && value.size() < maxLength) {
            value.push_back(char(m_event.value));
            changed = true;
        } else if (m_event.type == EventType::KeyDown && m_event.value == kVkBack) {
            if (!value.empty()) {
                value.pop_back();
                changed = true;
            }
            m_keyConsumed = true;
        } else if (m_event.type == EventType::KeyDown && (m_event.value == kVkReturn || m_event.value == kVkEscape)) {
            m_focus = 0;
            m_keyConsumed = true;
        }
    }
    fill(r, m_focus == id ? m_theme.buttonHot : m_theme.field);
    const std::size_t fit = std::size_t(std::max<std::int32_t>(0, widthChars - (m_focus == id ? 1 : 0)));
    std::string_view shown(value);
    if (shown.size() > fit) shown.remove_prefix(shown.size() - fit);
    glyphs(r.x + 2 * m_scale, r.y + m_scale, shown, m_theme.text, r.w - 2 * m_scale);
    if (m_focus == id) {
        const std::int32_t cx = r.x + 2 * m_scale + std::int32_t(shown.size()) * m_metrics.cellWidth * m_scale;
        m_draws.rect({f32(cx), f32(r.y + m_scale), f32(m_scale), f32(r.h - 2 * m_scale)}, m_theme.accent);
    }
    return changed;
}

bool ImmediateUi::widgetRect(std::string_view idText, IRect& out) const {
    const u32 id = hashId(idText);
    for (const auto& w : m_lastWidgets)
        if (w.id == id) {
            out = w.rect;
            return true;
        }
    return false;
}

} // namespace fuse::ui::immediate
