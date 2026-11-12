// FUSE UI widgets (plan §5). Tier A: Label, Button, ToolButton, CheckBox, RadioButton, Switch, Slider, ScrollBar,
// SpinBox, ComboBox, LineEdit, ProgressBar, Dial, Image, Separator, Link.
//
// Every widget is a registered WidgetClass (plain function table) over core nodes; state lives in the node (value,
// range, checked, text, edit buffer). Styling comes from the theme sheet: `color` = text, `background` = face,
// `background2` = accent / indicator fill (when no gradient is set), `border-color` = outline.
#pragma once

#include <span>

#include <string>
#include <vector>

#include "fuse/ui/ui.hpp"

#include <string_view>

namespace fuse::ui::widgets {

/// Registers every Tier A class with the context's registry.
void registerTierA(UiContext& ui);
/// The default dark theme (theme tokens in :root). Style sheet text, lives for the program.
std::string_view defaultThemeCss();
/// Registers the Tier B classes (popup, menu, menuitem, menubar, menubutton, listview, treeview, tabbar, tooltip).
void registerTierB(UiContext& ui);
/// registerTierA + registerTierB + default theme. (With Tier B registered, combo boxes open a popup list on click.)
void install(UiContext& ui);
/// Registers only the named widget classes (a cooked registration list, see fuse/ui/doc/cook.hpp) plus the default
/// theme. Names the context already has (core panel / label) are skipped. Returns how many names are unknown.
u32 installClasses(UiContext& ui, std::span<const std::string_view> classes, bool theme = true);

NodeHandle label(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle button(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle toolButton(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle checkBox(UiContext& ui, NodeHandle parent, std::string_view text, bool checked = false, std::string_view id = {});
NodeHandle radioButton(UiContext& ui, NodeHandle parent, std::string_view text, bool checked = false, std::string_view id = {});
NodeHandle toggleSwitch(UiContext& ui, NodeHandle parent, std::string_view text, bool on = false, std::string_view id = {});
NodeHandle slider(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, f32 step = 0.f, std::string_view id = {});
NodeHandle scrollBar(UiContext& ui, NodeHandle parent, bool vertical, std::string_view id = {});
NodeHandle spinBox(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, f32 step = 1.f, std::string_view id = {});
/// Items separated by '\n'; value() is the selected index. (Tier A: cycles in place; the popup list is Tier B.)
NodeHandle comboBox(UiContext& ui, NodeHandle parent, std::string_view items, u32 selected = 0, std::string_view id = {});
NodeHandle lineEdit(UiContext& ui, NodeHandle parent, std::string_view text = {}, std::string_view id = {});
NodeHandle progressBar(UiContext& ui, NodeHandle parent, f32 value01, std::string_view id = {});
NodeHandle dial(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, std::string_view id = {});
/// `imageIndex` is the CpuRasterizer / renderer image index (stored as user data).
NodeHandle image(UiContext& ui, NodeHandle parent, u32 imageIndex, f32 w, f32 h, std::string_view id = {});
NodeHandle separator(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle link(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle panel(UiContext& ui, NodeHandle parent, std::string_view id = {});

/// Selected item text of a combo box.
std::string_view comboText(const UiContext& ui, NodeHandle combo);

// ---- Tier B (plan §5) ------------------------------------------------------------------------------------------------
// Popups, menus, dialogs and tooltips live in the overlay layer: a core "layer" node kept as the root's last child,
// laid out over the viewport and outside the flow. A popup is a viewport-sized scope node (the scrim) holding the
// panel; outside clicks and Escape close it, focus is trapped in it (UiContext::setModal) and returns to the owner.

/// The overlay layer (created on first use; moved back to the root's last child on every call).
NodeHandle overlayLayer(UiContext& ui);

/// Opens a popup menu at `at` (context coordinates). Items separated by '\n'; "-" is a separator. Picking item i
/// emits Submitted {value = i, text} on the menu scope and on `owner` (when alive), then closes. Returns the scope.
NodeHandle popupMenu(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, std::string_view id = {});
/// Closes a popup scope (or the scope containing `n`): releases the modal trap, refocuses the owner, destroys it on
/// the next update (dialogs are hidden instead and can be reopened).
void closePopup(UiContext& ui, NodeHandle n);
/// True while a popup scope is open.
bool popupOpen(const UiContext& ui, NodeHandle scope);

/// A horizontal menu bar; menuBarMenu adds a title button whose popup lists `items` (Submitted re-emitted on it).
NodeHandle menuBar(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle menuBarMenu(UiContext& ui, NodeHandle bar, std::string_view title, std::string_view items, std::string_view id = {});

/// Modal dialog: a hidden scope in the overlay layer with a centred card (title + body). Add widgets to dialogBody.
NodeHandle dialog(UiContext& ui, std::string_view title, std::string_view id = {});
NodeHandle dialogBody(const UiContext& ui, NodeHandle dlg);
void openDialog(UiContext& ui, NodeHandle dlg);
void closeDialog(UiContext& ui, NodeHandle dlg);

/// Shows `text` under `node` after `delay` seconds of hover; hides on leave. Returns the tooltip node.
NodeHandle setTooltip(UiContext& ui, NodeHandle node, std::string_view text, f32 delay = 0.5f);

/// Virtualised single-selection list (items '\n'-separated; value() = selected index, -1 = none). Only visible rows
/// are painted. Selecting emits ValueChanged and TextChanged {item}; Enter emits Submitted.
NodeHandle listView(UiContext& ui, NodeHandle parent, std::string_view items, i32 selected = -1, std::string_view id = {});
std::string_view listItem(const UiContext& ui, NodeHandle list, u32 index);
u32 listFirstVisible(const UiContext& ui, NodeHandle list);

/// Tree view: one line per node, leading tabs give the depth. value() = selected line. Rows 0..63 can collapse
/// (expansion state is a 64-bit mask; deeper rows stay expanded). Toggling emits Toggled {value = line, flag = open}.
NodeHandle treeView(UiContext& ui, NodeHandle parent, std::string_view lines, std::string_view id = {});
bool treeExpanded(const UiContext& ui, NodeHandle tree, u32 line);
void setTreeExpanded(UiContext& ui, NodeHandle tree, u32 line, bool open);
/// Number of rows currently shown (collapsed subtrees skipped).
u32 treeVisibleRows(const UiContext& ui, NodeHandle tree);

/// Tab bar (titles '\n'-separated; value() = current tab) and a tab widget (tab bar + one page per title; only the
/// current page is displayed).
NodeHandle tabBar(UiContext& ui, NodeHandle parent, std::string_view titles, u32 current = 0, std::string_view id = {});
NodeHandle tabWidget(UiContext& ui, NodeHandle parent, std::string_view titles, std::string_view id = {});
NodeHandle tabPage(const UiContext& ui, NodeHandle tabs, u32 index);
NodeHandle tabWidgetBar(const UiContext& ui, NodeHandle tabs);

/// Scroll area: a column panel with overflow: scroll (wheel scrolls; role ScrollArea).
NodeHandle scrollArea(UiContext& ui, NodeHandle parent, std::string_view id = {});

// ---- Tier B: splitters, docks, item models -----------------------------------------------------------------------
/// Splitter: panes separated by draggable handles (Row = side by side, Column = stacked). A pane with size > 0 keeps
/// that many px (dragging resizes it); size 0 panes share the rest. Dragging emits ValueChanged {px} on the splitter.
NodeHandle splitter(UiContext& ui, NodeHandle parent, bool stacked = false, std::string_view id = {});
NodeHandle splitterPane(UiContext& ui, NodeHandle split, f32 size = 0.f, std::string_view id = {});

/// Dock area: left / right / bottom regions around a centre, each resizable by splitter handles and hidden while
/// empty. Dock widgets are titled panels with a close button; several in one region stack.
enum class DockSide : u8 { Left, Right, Bottom, Center };
NodeHandle dockArea(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle dockRegion(const UiContext& ui, NodeHandle area, DockSide side);
NodeHandle dockWidget(UiContext& ui, NodeHandle area, std::string_view title, DockSide side, std::string_view id = {});
NodeHandle dockBody(const UiContext& ui, NodeHandle dock);
void dockTo(UiContext& ui, NodeHandle dock, DockSide side);
void dockClose(UiContext& ui, NodeHandle dock); ///< hides (dockTo re-shows); emits Toggled {flag = false}

/// Item model: a function table over caller data (no inheritance). `data` may return a view into `scratch`.
struct ItemModel {
    const void* user = nullptr;
    u32 columns = 1;
    u32 (*rows)(const void* user) = nullptr;
    std::string_view (*data)(const void* user, u32 row, u32 col, std::string& scratch) = nullptr;
    std::string_view (*header)(const void* user, u32 col) = nullptr; ///< optional
};
/// A ready-made row/column string model (owns its cells). sortBy reorders rows (stable, numeric when both parse).
class StringTableModel {
public:
    explicit StringTableModel(std::string_view headersTabSeparated);
    StringTableModel(const StringTableModel&) = delete; ///< the model table points at this object
    StringTableModel& operator=(const StringTableModel&) = delete;
    void addRow(std::string_view cellsTabSeparated);
    void clear() { m_cells.clear(); m_rows = 0; }
    void sortBy(u32 col, bool ascending);
    u32 rows() const { return m_rows; }
    std::string_view cell(u32 row, u32 col) const { return m_cells[std::size_t(row) * m_cols + col]; }
    const ItemModel& model() const { return m_model; }

private:
    std::vector<std::string> m_headers, m_cells;
    u32 m_cols = 1, m_rows = 0;
    ItemModel m_model;
};
/// Virtualised table view over a model (not owned; must outlive the view): header row (click sorts: Toggled
/// {value = column, flag = ascending}; drag a header edge to resize), single row selection (value() = row, -1 none;
/// ValueChanged; Enter -> Submitted), wheel / keys / scrollbar drag scroll. Only visible rows are read and painted.
NodeHandle tableView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id = {});
/// Call after the model's rows change (re-clamps the scroll, repaints).
void tableModelChanged(UiContext& ui, NodeHandle table);
u32 tableFirstVisible(const UiContext& ui, NodeHandle table);
f32 tableColumnWidth(const UiContext& ui, NodeHandle table, u32 col);
void setTableColumnWidth(UiContext& ui, NodeHandle table, u32 col, f32 px);
/// List view over column 0 of a model (shares the table machinery, no header).
NodeHandle modelListView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id = {});

/// Dock layout persistence (plan §5 "saved layouts"): a small text blob with the side region sizes and, per dock
/// widget with an id, its side, order in the region and open state. Restoring moves / opens / closes the named docks
/// and resizes the regions; docks the blob does not name keep their place, names with no dock are ignored. A malformed
/// blob changes nothing and returns false.
std::string dockSaveLayout(const UiContext& ui, NodeHandle area);
bool dockRestoreLayout(UiContext& ui, NodeHandle area, std::string_view layout);

// ---- Tier C: delegates, proxies, grid view (plan §5) ------------------------------------------------------------------
/// Cell delegate: custom painting for one table column (the view still owns selection, scrolling and the row stripe).
struct CellDelegate {
    const void* user = nullptr;
    void (*paint)(const void* user, const PaintContext& pc, DrawList& dl, Rect cell, u32 row, u32 col,
                  std::string_view text, bool selected) = nullptr;
};
/// Sets (or with nullptr clears) the delegate for `col`. The delegate must outlive the view.
void setTableDelegate(UiContext& ui, NodeHandle table, u32 col, const CellDelegate* delegate);

/// Sort / filter proxy over a source model (not owned). Identity (no row map, no memory) until a filter or sort is
/// set; filtering is a case-insensitive substring match on one column. Call invalidate() after the source changes.
class SortFilterProxy {
public:
    explicit SortFilterProxy(const ItemModel* source);
    SortFilterProxy(const SortFilterProxy&) = delete;
    SortFilterProxy& operator=(const SortFilterProxy&) = delete;
    void setFilter(u32 col, std::string_view needle); ///< empty needle = no filter
    void sortBy(i32 col, bool ascending);              ///< col < 0 = source order
    void invalidate();
    u32 rows() const;
    u32 sourceRow(u32 row) const { return m_identity ? row : m_map[row]; }
    const ItemModel& model() const { return m_model; }

private:
    const ItemModel* m_src;
    ItemModel m_model;
    std::vector<u32> m_map;
    std::string m_needle;
    u32 m_filterCol = 0;
    i32 m_sortCol = -1;
    bool m_asc = true, m_identity = true;
};

/// Virtualised icon / tile grid over column 0 of a model: tiles of tileW x tileH flow left to right, rows scroll.
/// value() = selected item (-1 none); arrows move in 2D, Enter -> Submitted. Only visible tiles are read.
NodeHandle gridView(UiContext& ui, NodeHandle parent, const ItemModel* model, f32 tileW = 96.f, f32 tileH = 72.f,
                    std::string_view id = {});
u32 gridColumns(const UiContext& ui, NodeHandle grid);
u32 gridFirstVisibleRow(const UiContext& ui, NodeHandle grid);

// ---- Tier D: rich and pro widgets (plan §5) ----------------------------------------------------------------------------
/// Property grid (reflection-driven inspector): name / editor rows built from descriptors. Group rows fold the rows
/// after them (until the next group). Any editor change emits Submitted {value = row index, text = name} on the grid.
enum class PropKind : u8 { Group, Bool, Number, Text, Enum, Color };
struct PropertyDesc {
    std::string_view name;
    PropKind kind = PropKind::Text;
    f32 lo = 0.f, hi = 100.f, step = 1.f; ///< Number
    std::string_view options{};           ///< Enum: '\n'-separated
};
NodeHandle propertyGrid(UiContext& ui, NodeHandle parent, std::string_view id = {});
/// Adds a row; `value` is "true"/"false", a number, text, an enum index or "#RRGGBB[AA]". Returns the editor node.
NodeHandle propertyRow(UiContext& ui, NodeHandle grid, const PropertyDesc& desc, std::string_view value = {});
/// Current value of row `index` in the same text form propertyRow takes.
std::string propertyValue(const UiContext& ui, NodeHandle grid, u32 index);
u32 propertyCount(const UiContext& ui, NodeHandle grid);
void setPropertyGroupOpen(UiContext& ui, NodeHandle grid, u32 groupIndex, bool open);

/// HSV colour picker: saturation / value square + hue bar + alpha bar. text() = "#RRGGBBAA"; drags and arrow keys
/// emit ValueChanged {value = hue} and TextChanged {text}.
NodeHandle colorPicker(UiContext& ui, NodeHandle parent, Color initial, std::string_view id = {});
Color colorPickerColor(const UiContext& ui, NodeHandle picker);
void setColorPickerColor(UiContext& ui, NodeHandle picker, Color c);

/// Month calendar: value() = days since 1970-01-01. Arrows move by a day / week, PageUp / PageDown by a month, the
/// header arrows page months, clicking a day selects it (ValueChanged), Enter -> Submitted.
NodeHandle calendar(UiContext& ui, NodeHandle parent, int year, int month, int day, std::string_view id = {});
void calendarDate(const UiContext& ui, NodeHandle cal, int& year, int& month, int& day);
i32 daysFromCivil(int y, int m, int d);
void civilFromDays(i32 z, int& y, int& m, int& d);

/// Chart: line, bar, area or scatter series over a shared x index, auto-scaled y axis with grid lines. Hovering
/// sets value() to the nearest x index (-1 outside).
enum class ChartKind : u8 { Line, Bar, Area, Scatter };
NodeHandle chart(UiContext& ui, NodeHandle parent, ChartKind kind, std::string_view id = {});
void chartSetSeries(UiContext& ui, NodeHandle chart, u32 index, std::string_view name, std::span<const f32> values, Color color);
u32 chartSeriesCount(const UiContext& ui, NodeHandle chart);

/// Markdown view: headings (#, ##, ###), paragraphs, '-' / '*' bullets, numbered items, ``` code blocks and '>' quotes
/// become styled label children (classes md-h1..md-h3, md-p, md-li, md-code, md-quote). Inline ** and ` are stripped.
NodeHandle markdownView(UiContext& ui, NodeHandle parent, std::string_view markdown, std::string_view id = {});
void setMarkdown(UiContext& ui, NodeHandle view, std::string_view markdown);

/// Registers the Tier C / D classes (gridview, colorpicker, calendar, chart); install() calls it.
void registerTierCD(UiContext& ui);

// ---- Tier D (2): pro widgets (plan §5) ---------------------------------------------------------------------------------
/// Font picker: family list + size spin box + live preview line. Families default to the bundled faces when empty
/// ('\n'-separated otherwise). Any change emits Submitted {text = family, value = size} on the picker.
NodeHandle fontPicker(UiContext& ui, NodeHandle parent, std::string_view families = {}, f32 size = 14.f, std::string_view id = {});
std::string fontPickerFamily(const UiContext& ui, NodeHandle picker);
f32 fontPickerSize(const UiContext& ui, NodeHandle picker);

/// File dialog (modal, built on dialog()): path field, "Up" button, directory listing (folders first, then files
/// matching `filter`, e.g. "*.fuseui;*.png", empty = all), file-name field, OK / Cancel. Activating a folder enters
/// it; OK (or Enter / double activation on a file) emits Submitted {text = chosen path} on the dialog and closes it;
/// Cancel emits Submitted {text = ""}. `save` mode accepts names that do not exist yet.
NodeHandle fileDialog(UiContext& ui, std::string_view title, std::string_view directory, std::string_view filter = {},
                      bool save = false, std::string_view id = {});
bool fileDialogNavigate(UiContext& ui, NodeHandle dlg, std::string_view directory);
std::string fileDialogDirectory(const UiContext& ui, NodeHandle dlg);
u32 fileDialogEntryCount(const UiContext& ui, NodeHandle dlg);
std::string fileDialogEntry(const UiContext& ui, NodeHandle dlg, u32 index); ///< names; folders end in '/'
/// Activates entry `index` (folder: enter; file: accept). Returns the accepted path or "".
std::string fileDialogActivate(UiContext& ui, NodeHandle dlg, u32 index);

/// Node graph editor: nodes with typed input / output pins, bezier links, drag nodes, drag from an output pin to an
/// input pin to link (types must match unless either is 0 = any; an input takes one link; cycles are refused),
/// right-drag or middle-drag pans, wheel zooms, Delete removes the selected node. value() = selected node (-1).
/// Link changes emit ValueChanged {value = link count}.
struct GraphPin { std::string_view name; u32 type = 0; };
NodeHandle nodeGraph(UiContext& ui, NodeHandle parent, std::string_view id = {});
u32 nodeGraphAddNode(UiContext& ui, NodeHandle graph, std::string_view title, Vec2 pos, std::span<const GraphPin> inputs,
                     std::span<const GraphPin> outputs);
bool nodeGraphConnect(UiContext& ui, NodeHandle graph, u32 fromNode, u32 outPin, u32 toNode, u32 inPin);
bool nodeGraphDisconnect(UiContext& ui, NodeHandle graph, u32 toNode, u32 inPin);
bool nodeGraphRemoveNode(UiContext& ui, NodeHandle graph, u32 node);
u32 nodeGraphNodeCount(const UiContext& ui, NodeHandle graph);
u32 nodeGraphLinkCount(const UiContext& ui, NodeHandle graph);
Vec2 nodeGraphNodePos(const UiContext& ui, NodeHandle graph, u32 node);
/// Screen position of a pin's dot (tests, tutorials). `output` picks the side.
Vec2 nodeGraphPinPos(const UiContext& ui, NodeHandle graph, u32 node, u32 pin, bool output);
/// Topological order of live nodes (empty when the graph has a cycle, which connect never allows).
std::vector<u32> nodeGraphTopoOrder(const UiContext& ui, NodeHandle graph);

/// Curve editor: one float curve of keys (t, v) with linear / smooth (Catmull-Rom style, monotone in t) / step
/// interpolation. Click empty space adds a key, drag moves keys (t kept between neighbours), Delete removes the
/// selected key. Changes emit ValueChanged {value = key count}.
enum class CurveInterp : u8 { Linear, Smooth, Step };
NodeHandle curveEditor(UiContext& ui, NodeHandle parent, f32 tMin = 0.f, f32 tMax = 1.f, f32 vMin = 0.f, f32 vMax = 1.f,
                       std::string_view id = {});
u32 curveAddKey(UiContext& ui, NodeHandle curve, f32 t, f32 v, CurveInterp interp = CurveInterp::Smooth);
u32 curveKeyCount(const UiContext& ui, NodeHandle curve);
Vec2 curveKey(const UiContext& ui, NodeHandle curve, u32 index); ///< (t, v)
f32 curveEvaluate(const UiContext& ui, NodeHandle curve, f32 t);

/// Timeline: named tracks with clips [start, end), a time ruler and a playhead. Dragging the ruler scrubs
/// (ValueChanged {value = time}); dragging a clip moves it (snapped to `snap` seconds when > 0); wheel zooms.
/// value() = playhead time in seconds.
NodeHandle timeline(UiContext& ui, NodeHandle parent, f32 duration, std::string_view id = {});
u32 timelineAddTrack(UiContext& ui, NodeHandle tl, std::string_view name);
u32 timelineAddClip(UiContext& ui, NodeHandle tl, u32 track, f32 start, f32 end, std::string_view label, Color color);
void timelineSetSnap(UiContext& ui, NodeHandle tl, f32 seconds);
/// Visible range [start, start + span) seconds, scaled to the lane width (span 0 = free zoom: wheel / pxPerSec).
/// With a range set, Ctrl+wheel zooms about the pointer and Shift+wheel pans, then emit Event {value = 1}.
void timelineSetView(UiContext& ui, NodeHandle tl, f32 startSec, f32 spanSec);
Vec2 timelineView(const UiContext& ui, NodeHandle tl); ///< (start, span) seconds
Vec2 timelineClip(const UiContext& ui, NodeHandle tl, u32 track, u32 clip); ///< (start, end)
/// Clips under time `t` on every track (sequencer queries).
u32 timelineActiveClips(const UiContext& ui, NodeHandle tl, f32 t);

/// Viewport widget: shows an externally rendered image (a rasteriser / GPU image index, e.g. the engine's scene
/// colour target - zero copy) letterboxed to its aspect ratio, and forwards input in image pixel coordinates.
/// Each forwarded input emits SignalKind::Event {value = EventType}; read it with viewportLastInput().
struct ViewportInput { EventType type = EventType::PointerMove; Vec2 pos; MouseButton button = MouseButton::Left; Vec2 wheel; Key key = Key::None; };
NodeHandle viewportWidget(UiContext& ui, NodeHandle parent, u32 imageIndex, u32 imageW, u32 imageH, std::string_view id = {});
void viewportSetImage(UiContext& ui, NodeHandle vp, u32 imageIndex, u32 imageW, u32 imageH);
Rect viewportImageRect(const UiContext& ui, NodeHandle vp); ///< letterboxed rect in context coordinates
const ViewportInput& viewportLastInput(const UiContext& ui, NodeHandle vp);
u32 viewportInputCount(const UiContext& ui, NodeHandle vp);
/// Vector overlay painted over the viewport image (gizmos, selection markers), in image pixel coordinates; it is part
/// of the UI draw list, so it composes over an external (zero-copy) image as well as a CPU one.
struct ViewportOverlay {
    struct Line { Vec2 a, b; Color color; f32 width = 2.f; };
    struct Dot { Vec2 center; f32 radius = 4.f; Color color; Color border{}; f32 borderWidth = 0.f; };
    std::vector<Line> lines;
    std::vector<Dot> dots;
    /// Frame around the whole viewport (frameWidth > 0) and a centred top badge (badge non-empty), widget space:
    /// the Qt play-mode frame (green PLAYING / amber PAUSED).
    Color frame{};
    f32 frameWidth = 0.f;
    std::string badge;
    Color badgeColor{}, badgeText{};
};
void viewportSetOverlay(UiContext& ui, NodeHandle vp, ViewportOverlay overlay);
const ViewportOverlay& viewportOverlay(const UiContext& ui, NodeHandle vp);

/// Code editor: multi-line text with a line-number gutter, syntax colouring (Lua / C-like keywords, strings,
/// numbers, comments), caret + selection with Shift, Tab inserts spaces, auto-indent on Enter, undo (Ctrl+Z),
/// copy / cut / paste, vertical scrolling. text() = the document. Edits emit TextChanged.
enum class CodeLanguage : u8 { Plain, Lua, Cpp, Css };
NodeHandle codeEdit(UiContext& ui, NodeHandle parent, std::string_view text = {}, CodeLanguage lang = CodeLanguage::Lua,
                    std::string_view id = {});
void codeEditSetText(UiContext& ui, NodeHandle ed, std::string_view text);
std::string codeEditText(const UiContext& ui, NodeHandle ed);
u32 codeEditLineCount(const UiContext& ui, NodeHandle ed);
void codeEditCaret(const UiContext& ui, NodeHandle ed, u32& line, u32& column);
void codeEditSetCaret(UiContext& ui, NodeHandle ed, u32 line, u32 column);
/// Token class of the character at (line, column): 0 text, 1 keyword, 2 string, 3 number, 4 comment.
u8 codeEditTokenAt(const UiContext& ui, NodeHandle ed, u32 line, u32 column);

/// Date-time edit: "YYYY-MM-DD HH:MM" in fields; Left / Right pick a field, Up / Down step it (with carry and
/// month-length clamping), digits type into it, the button (or Space) opens a calendar popup. value() = days since
/// 1970-01-01 (exact in f32); dateTimeEditMinutes() = minutes since 1970-01-01 00:00 (civil time, no zone). Changes
/// emit ValueChanged.
NodeHandle dateTimeEdit(UiContext& ui, NodeHandle parent, int year, int month, int day, int hour = 0, int minute = 0,
                        std::string_view id = {});
void dateTimeEditValue(const UiContext& ui, NodeHandle ed, int& year, int& month, int& day, int& hour, int& minute);
void setDateTimeEditValue(UiContext& ui, NodeHandle ed, int year, int month, int day, int hour, int minute);
i64 dateTimeEditMinutes(const UiContext& ui, NodeHandle ed);
u32 dateTimeEditField(const UiContext& ui, NodeHandle ed); ///< 0 year .. 4 minute

/// Video widget: plays a VideoSource (function table). Theora .ogv files: fuse_ui_video's `video::TheoraVideo::source()`
/// (fuse/ui/video/theora_video.hpp); `testPatternSource()` is a built-in moving pattern. Audio sync is not done. Space toggles play, the scrubber seeks.
/// value() = current frame. Frames are pulled from `frame()` into an image the rasteriser shows.
struct VideoSource {
    void* user = nullptr;
    u32 width = 0, height = 0, frames = 0;
    f32 fps = 30.f;
    /// Fills `rgba` (width * height premultiplied RGBA8, R in the low byte) with frame `index`.
    void (*frame)(void* user, u32 index, u32* rgba) = nullptr;
};
const VideoSource& testPatternSource();
NodeHandle videoWidget(UiContext& ui, NodeHandle parent, const VideoSource* source, std::string_view id = {});
/// The rasteriser image index the widget paints (register it with CpuRasterizer::addImage via videoImage()).
const Image* videoImage(const UiContext& ui, NodeHandle video);
void videoSetImageIndex(UiContext& ui, NodeHandle video, u32 index);
void videoPlay(UiContext& ui, NodeHandle video, bool play);
bool videoPlaying(const UiContext& ui, NodeHandle video);
void videoSeek(UiContext& ui, NodeHandle video, u32 frame);
u32 videoFrame(const UiContext& ui, NodeHandle video);
u32 videoDecodedFrames(const UiContext& ui, NodeHandle video);

/// Registers the Tier D (2) classes; install() calls it.
void registerTierD2(UiContext& ui);

} // namespace fuse::ui::widgets
