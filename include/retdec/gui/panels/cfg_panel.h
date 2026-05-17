/**
 * @file include/retdec/gui/panels/cfg_panel.h
 * @brief Interactive control-flow graph visualiser (Task 51).
 *
 * Features
 * --------
 *  - BasicBlockItem   : rounded rect with address header + up-to-3 instruction preview
 *  - CFGEdgeItem      : coloured, arrowed paths per EdgeKind
 *  - LoopRegionItem   : translucent coloured outline over loop bodies
 *  - CFGScene         : Sugiyama layered layout, >200-node chain compression
 *  - MiniMapView      : scaled overview with draggable viewport indicator
 *  - CFGView          : Ctrl+scroll zoom, middle-click pan, Ctrl+F fit, Ctrl+0 reset
 *  - CFGPanel         : toolbar, minimap overlay, SVG/PNG export, TriPane navigation
 */

#ifndef RETDEC_GUI_PANELS_CFG_H
#define RETDEC_GUI_PANELS_CFG_H

#include "retdec/gui/panels/panel_base.h"

#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
QT_END_NAMESPACE

namespace retdec::gui::panels {

// ─── Data model ───────────────────────────────────────────────────────────────

/** Edge classification; determines colour and dash pattern. */
enum class EdgeKind : uint8_t {
    FallThrough,         ///< solid grey
    TrueBranch,          ///< solid green
    FalseBranch,         ///< solid red
    BackEdge,            ///< orange dashed arc (rendered as curve to the left)
    ExceptionEdge,       ///< solid purple
    UnresolvedIndirect,  ///< red dashed
};

/** One disassembly instruction inside a basic block. */
struct BlockInstr {
    uint64_t address = 0;
    QString  text;   ///< "mnemonic  operands"
};

/** Full data for one basic block node. */
struct BasicBlockData {
    uint64_t               id          = 0;   ///< unique id (e.g. start address)
    uint64_t               address     = 0;   ///< start address (for display / navigation)
    std::vector<BlockInstr> instrs;            ///< all instructions; preview shows first 3
    bool                   isLoopHeader = false;
    int                    loopId      = -1;  ///< ≥0 means part of this loop
    bool                   isEntry     = false;
    bool                   isExit      = false;
};

/** One directed edge between two basic blocks. */
struct CFGEdgeData {
    uint64_t from = 0;
    uint64_t to   = 0;
    EdgeKind kind = EdgeKind::FallThrough;
};

// ─── BasicBlockItem ───────────────────────────────────────────────────────────

/**
 * @brief Clickable, hoverable rounded-rect representing one basic block.
 *
 * Layout (top→bottom):
 *   [address header]
 *   [instr 0]
 *   [instr 1]
 *   [instr 2]
 *   … (truncated if more)
 */
class BasicBlockItem : public QGraphicsObject {
    Q_OBJECT
public:
    explicit BasicBlockItem(const BasicBlockData& data,
                            QGraphicsItem*        parent = nullptr);

    QRectF boundingRect() const override;
    void   paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override;

    uint64_t blockId()      const { return data_.id; }
    uint64_t blockAddress() const { return data_.address; }

    /** Centre of the bottom edge — attach outgoing edges here. */
    QPointF bottomCentre() const;
    /** Centre of the top edge — attach incoming edges here. */
    QPointF topCentre()    const;

signals:
    void clicked(uint64_t blockId);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent*)  override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent*)  override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent*)  override;

private:
    qreal totalHeight() const;

    BasicBlockData data_;
    bool           hovered_ = false;

    static constexpr qreal kWidth      = 230.0;
    static constexpr qreal kHeaderH    = 24.0;
    static constexpr qreal kInstrH     = 16.0;
    static constexpr int   kMaxPreview = 3;
    static constexpr qreal kPadX       = 10.0;
    static constexpr qreal kRadius     = 7.0;
    static constexpr qreal kFooterH    = 4.0;  // bottom padding
};

// ─── CFGEdgeItem ─────────────────────────────────────────────────────────────

/**
 * @brief Directed edge with colour-coded type and arrowhead.
 *
 * Back edges are rendered as a wide left-side arc so they don't
 * cross over forward-edge paths.
 */
class CFGEdgeItem : public QGraphicsPathItem {
public:
    CFGEdgeItem(QPointF from, QPointF to, EdgeKind kind,
                bool isCurvedBack = false,
                QGraphicsItem* parent = nullptr);

    static QColor colorForKind(EdgeKind kind);

private:
    void buildPath(QPointF from, QPointF to, EdgeKind kind, bool isCurvedBack);
    void addArrowHead(QPainterPath& path, QPointF tip, QPointF incoming);
};

// ─── LoopRegionItem ──────────────────────────────────────────────────────────

/** Translucent coloured rectangle drawn behind a loop body. */
class LoopRegionItem : public QGraphicsRectItem {
public:
    LoopRegionItem(QRectF bounds, QColor color, QGraphicsItem* parent = nullptr);
};

// ─── MiniMapView ─────────────────────────────────────────────────────────────

/**
 * @brief Scaled overview of the whole CFGScene.
 *
 * Draws a translucent rectangle showing the current main-view viewport.
 * Clicking / dragging inside the minimap pans the main view.
 */
class MiniMapView : public QGraphicsView {
    Q_OBJECT
public:
    MiniMapView(QGraphicsScene* scene, QGraphicsView* mainView,
                QWidget* parent = nullptr);

    /** Call after the main view's viewport changes to redraw the indicator. */
    void syncViewport();

protected:
    void paintEvent(QPaintEvent*)   override;
    void mousePressEvent(QMouseEvent*)  override;
    void mouseMoveEvent(QMouseEvent*)   override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void panMainViewTo(QPoint miniMapPos);

    QGraphicsView* mainView_  = nullptr;
    bool           dragging_  = false;
};

// ─── CFGView ─────────────────────────────────────────────────────────────────

/**
 * @brief Main graph viewport.
 *
 * Keyboard shortcuts:
 *   Ctrl+scroll  – zoom in/out
 *   Ctrl+F       – fit to window
 *   Ctrl+0       – reset zoom to 100 %
 *   Middle-drag  – pan
 */
class CFGView : public QGraphicsView {
    Q_OBJECT
public:
    explicit CFGView(QGraphicsScene* scene, QWidget* parent = nullptr);

    void setMiniMap(MiniMapView* mm) { miniMap_ = mm; }

    void fitGraph();
    void resetZoom();

signals:
    void viewportMoved();

protected:
    void wheelEvent(QWheelEvent*)        override;
    void mousePressEvent(QMouseEvent*)   override;
    void mouseMoveEvent(QMouseEvent*)    override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*)       override;
    void scrollContentsBy(int, int)      override;
    void resizeEvent(QResizeEvent*)      override;

private:
    void repositionMiniMap();

    bool         panning_  = false;
    QPoint       lastPan_;
    MiniMapView* miniMap_  = nullptr;
};

// ─── CFGScene ────────────────────────────────────────────────────────────────

/**
 * @brief Holds all CFG graphics items and performs layout.
 *
 * Layout algorithm (Sugiyama, simplified):
 *  1. DFS back-edge identification.
 *  2. BFS rank assignment (back edges skipped).
 *  3. Barycentric crossing minimisation (2 sweep directions).
 *  4. Uniform x spacing, rank-based y spacing.
 *
 * If node count > 200 the graph is *compressed* first:
 * non-branching chains (in-degree 1, out-degree 1) are collapsed into
 * a single grey "chain" node showing the instruction count.
 * Clicking a compressed node is a no-op (expand-on-click is future work).
 */
class CFGScene : public QGraphicsScene {
    Q_OBJECT
public:
    explicit CFGScene(QObject* parent = nullptr);

    /** Replace current graph with new CFG data. Triggers relayout. */
    void loadCFG(const std::vector<BasicBlockData>& blocks,
                 const std::vector<CFGEdgeData>&    edges);

    void clearGraph();

    bool isCompressed() const { return compressed_; }
    int  nodeCount()    const { return static_cast<int>(blockItems_.size()); }

signals:
    void blockClicked(uint64_t blockId);

private:
    // ── Internal layout types ──────────────────────────────────────────────
    struct NodeLayout {
        uint64_t id    = 0;
        int      rank  = 0;   // row (level in hierarchy)
        int      order = 0;   // position within rank
        qreal    x     = 0;
        qreal    y     = 0;
    };

    // ── Layout steps ──────────────────────────────────────────────────────
    void identifyBackEdges(
        uint64_t entry,
        const std::unordered_map<uint64_t, std::vector<uint64_t>>& succ,
        std::unordered_set<uint64_t>& backFromTo) const;

    void assignRanks(
        uint64_t entry,
        const std::unordered_map<uint64_t, std::vector<uint64_t>>& succ,
        const std::unordered_set<uint64_t>& backSet,
        std::unordered_map<uint64_t, int>&  rank) const;

    void barycentricSweep(
        std::vector<std::vector<uint64_t>>& levels,
        const std::unordered_map<uint64_t, std::vector<uint64_t>>& pred,
        const std::unordered_map<uint64_t, std::vector<uint64_t>>& succ) const;

    // ── Chain compression ─────────────────────────────────────────────────
    std::vector<BasicBlockData> compressChains(
        const std::vector<BasicBlockData>&    in,
        const std::vector<CFGEdgeData>&       inEdges,
        std::vector<CFGEdgeData>&             outEdges) const;

    // ── Loop region overlay ───────────────────────────────────────────────
    void buildLoopRegions(
        const std::vector<BasicBlockData>&    blocks,
        const std::unordered_set<uint64_t>&  backSet,
        const std::unordered_map<uint64_t, std::vector<uint64_t>>& succ);

    // ── State ─────────────────────────────────────────────────────────────
    bool                                     compressed_ = false;
    std::unordered_map<uint64_t, BasicBlockItem*> blockItems_;

    // ── Layout constants ──────────────────────────────────────────────────
    static constexpr int   kCompressThreshold = 200;
    static constexpr qreal kLevelH            = 140.0;  // vertical spacing between ranks
    static constexpr qreal kNodeSpacingH      = 270.0;  // horizontal spacing
    static constexpr qreal kMargin            = 40.0;
};

// ─── CFGPanel ────────────────────────────────────────────────────────────────

/**
 * @brief Full CFG panel widget.
 *
 * Toolbar: function label, Fit, Reset Zoom, Export SVG, Export PNG.
 * Minimap overlaid in the top-right corner of the graph view.
 * Emits blockNavigationRequested() when a block is clicked.
 */
class CFGPanel : public PanelBase {
    Q_OBJECT
public:
    explicit CFGPanel(QWidget* parent = nullptr);

    /** Load or replace the displayed CFG. */
    void loadCFG(const std::vector<BasicBlockData>& blocks,
                 const std::vector<CFGEdgeData>&    edges);

    void clear() override;

public slots:
    void onFunctionSelected(uint64_t address, const QString& name);

signals:
    /** Emitted when the user clicks a basic block; carry its start address. */
    void blockNavigationRequested(uint64_t blockAddress);

private slots:
    void onFitView();
    void onResetZoom();
    void onExportSvg();
    void onExportPng();
    void onBlockClicked(uint64_t blockId);

private:
    void setupUI();

    QLabel*      funcLabel_   = nullptr;
    QPushButton* fitButton_   = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* svgButton_   = nullptr;
    QPushButton* pngButton_   = nullptr;
    QLabel*      infoLabel_   = nullptr;

    CFGScene*    scene_       = nullptr;
    CFGView*     graphView_   = nullptr;
    MiniMapView* miniMap_     = nullptr;

    /** blockId → start address (for navigation signal). */
    std::unordered_map<uint64_t, uint64_t> addressMap_;
};

} // namespace retdec::gui::panels
#endif // RETDEC_GUI_PANELS_CFG_H
