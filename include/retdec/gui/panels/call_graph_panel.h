/**
 * @file include/retdec/gui/panels/call_graph_panel.h
 * @brief Zoomable, filterable, interactive call-graph panel (Task 52).
 *
 * Features
 * --------
 *  - CallGraphNodeItem   : rounded rect, size ∝ instruction count
 *  - CallGraphEdgeItem   : directed arrow, weight ∝ call count, dashed for indirect
 *  - SccSuperNodeItem    : collapsed SCC with node count badge
 *  - ModuleClusterItem   : translucent backdrop per community/module group
 *  - Force-directed layout (spring embedding, 60 iterations)
 *  - Tarjan SCC detection; SCCs with >1 member collapsed into super-nodes
 *  - FilterBar           : name glob, module filter, min-call-count slider
 *  - HideLibraryFunctions toggle
 *  - FocusMode           : right-click → show only N-hop callers/callees
 *  - DOT export for Graphviz
 *  - Zoom (Ctrl+scroll), pan (middle-drag), Ctrl+F fit, Ctrl+0 reset
 */

#ifndef RETDEC_GUI_PANELS_CALL_GRAPH_H
#define RETDEC_GUI_PANELS_CALL_GRAPH_H

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
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
QT_END_NAMESPACE

namespace retdec::gui::panels {

// ─── Data model ───────────────────────────────────────────────────────────────

/** One function node in the call graph. */
struct CallGraphNode {
    uint64_t address    = 0;
    QString  name;
    int      instrCount = 0;   ///< used to scale node size
    bool     isLibrary  = false;
    int      moduleId   = -1;  ///< community / module id for clustering
};

/** One directed call edge. */
struct CallEdge {
    uint64_t callerAddress = 0;
    uint64_t calleeAddress = 0;
    int      callCount     = 1; ///< call frequency (weights arrow width)
    bool     isDirect      = true;
};

// ─── Graphics items ───────────────────────────────────────────────────────────

/** Clickable function node; right-click context menu for focus mode. */
class CallGraphNodeItem : public QGraphicsObject {
    Q_OBJECT
public:
    CallGraphNodeItem(const CallGraphNode& node, QColor moduleColor,
                      QGraphicsItem* parent = nullptr);

    QRectF boundingRect() const override;
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override;

    uint64_t address()  const { return node_.address; }
    QString  funcName() const { return node_.name; }
    QRectF   nodeRect() const;

    /** Dim the node (used when another node is focused). */
    void setDimmed(bool dim);

signals:
    void clicked(uint64_t address);
    void focusRequested(uint64_t address, int hops);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent*)   override;
    void contextMenuEvent(QGraphicsSceneContextMenuEvent*) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent*)   override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent*)   override;

private:
    qreal nodeWidth()  const;
    qreal nodeHeight() const;

    CallGraphNode node_;
    QColor        moduleColor_;
    bool          hovered_ = false;
    bool          dimmed_  = false;
};

/** Collapsed SCC super-node showing multiple functions as one box. */
class SccSuperNodeItem : public QGraphicsObject {
    Q_OBJECT
public:
    SccSuperNodeItem(const std::vector<uint64_t>& members,
                     const std::vector<QString>&   names,
                     QGraphicsItem* parent = nullptr);

    QRectF boundingRect() const override;
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override;

    const std::vector<uint64_t>& members() const { return members_; }

signals:
    void clicked(const std::vector<uint64_t>& members);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent*) override;

private:
    std::vector<uint64_t> members_;
    std::vector<QString>  names_;
};

/** Directed call edge with arrow and optional dashed style for indirect calls. */
class CallGraphEdgeItem : public QGraphicsPathItem {
public:
    CallGraphEdgeItem(QPointF from, QPointF to,
                      int callCount, bool isDirect,
                      QGraphicsItem* parent = nullptr);
private:
    void buildPath(QPointF from, QPointF to, int callCount, bool isDirect);
    void addArrow(QPainterPath& path, QPointF tip, QPointF incoming);
};

/** Translucent backdrop rectangle for a module cluster. */
class ModuleClusterItem : public QGraphicsRectItem {
public:
    ModuleClusterItem(QRectF bounds, QColor color, const QString& label,
                      QGraphicsItem* parent = nullptr);
    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override;
private:
    QString label_;
    QColor  color_;
};

// ─── CallGraphView ────────────────────────────────────────────────────────────

class CallGraphView : public QGraphicsView {
    Q_OBJECT
public:
    explicit CallGraphView(QGraphicsScene* scene, QWidget* parent = nullptr);
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
private:
    bool   panning_ = false;
    QPoint lastPan_;
};

// ─── CallGraphScene ───────────────────────────────────────────────────────────

/**
 * @brief Manages call graph layout and graphics items.
 *
 * Layout algorithm:
 *  1. Tarjan SCC — collapse SCCs with size > 1 into SccSuperNodeItem
 *  2. Force-directed spring embedding (60 iterations)
 *  3. Module cluster backdrops drawn at z = −2
 *
 * Filtering:
 *  - By name glob
 *  - By module id
 *  - By minimum call count
 *  - Hide library functions
 *
 * Focus mode:
 *  - Show only the N-hop neighbourhood of the focused node
 */
class CallGraphScene : public QGraphicsScene {
    Q_OBJECT
public:
    explicit CallGraphScene(QObject* parent = nullptr);

    void loadGraph(const std::vector<CallGraphNode>& nodes,
                   const std::vector<CallEdge>&      edges);

    void clearGraph();

    // Filtering
    void setNameFilter(const QString& glob);
    void setMinCallCount(int minCount);
    void setHideLibrary(bool hide);
    void setModuleFilter(int moduleId);   ///< -1 = show all

    // Focus mode
    void setFocusNode(uint64_t address, int hops);
    void clearFocus();

    // Export
    QString exportDot() const;

    int  visibleNodeCount() const;
    bool isFocused()        const { return focusAddress_ != 0; }

signals:
    void nodeClicked(uint64_t address);
    void focusRequested(uint64_t address, int hops);

private:
    // SCC detection
    struct SccResult {
        std::vector<std::vector<uint64_t>> sccs; // each scc is a group of addresses
    };
    SccResult computeSccs(
        const std::vector<uint64_t>& nodes,
        const std::unordered_map<uint64_t, std::vector<uint64_t>>& succ) const;

    // Force-directed layout
    struct LayoutNode {
        uint64_t addr = 0;
        qreal    x = 0, y = 0;
        qreal    fx = 0, fy = 0; // force accumulator
    };
    void forceDirectedLayout(std::vector<LayoutNode>& ln,
                              const std::unordered_map<uint64_t,
                                    std::vector<uint64_t>>& edges,
                              int iterations = 60) const;

    // Visibility
    void applyFilters();
    bool isNodeVisible(uint64_t addr) const;

    // Data
    std::vector<CallGraphNode>                         nodes_;
    std::vector<CallEdge>                              edges_;

    // Graphics items (keyed by address)
    std::unordered_map<uint64_t, CallGraphNodeItem*>   nodeItems_;
    std::unordered_map<uint64_t, SccSuperNodeItem*>    sccItems_;

    // Filter state
    QString  nameFilter_;
    int      minCallCount_  = 0;
    bool     hideLibrary_   = false;
    int      moduleFilter_  = -1;

    // Focus state
    uint64_t                    focusAddress_ = 0;
    int                         focusHops_    = 2;
    std::unordered_set<uint64_t> focusSet_;

    // Module colour palette
    static QColor moduleColor(int moduleId);

    static constexpr int   kMaxNodes          = 1000; // truncate display above this
    static constexpr qreal kMargin            = 60.0;
};

// ─── CallGraphPanel ───────────────────────────────────────────────────────────

/**
 * @brief Full call-graph panel widget.
 *
 * Toolbar: name filter | module filter | min-call-count | hide-library toggle
 *          | focus hops | Fit | Reset | Export DOT
 */
class CallGraphPanel : public PanelBase {
    Q_OBJECT
public:
    explicit CallGraphPanel(QWidget* parent = nullptr);

    void loadGraph(const std::vector<CallGraphNode>& nodes,
                   const std::vector<CallEdge>&      edges);

    /** Legacy compat. */
    void setCallGraph(const QStringList& functionNames,
                      const std::vector<CallEdge>& edges);

    void clear() override;

public slots:
    void onFunctionSelected(uint64_t address, const QString& name);

signals:
    void functionNavigationRequested(uint64_t address);

private slots:
    void onFitView();
    void onResetZoom();
    void onExportDot();
    void onNameFilterChanged(const QString& text);
    void onMinCallCountChanged(int value);
    void onHideLibraryToggled(bool hide);
    void onNodeClicked(uint64_t address);
    void onFocusRequested(uint64_t address, int hops);
    void onClearFocus();
    void onDepthChanged(int depth);

private:
    void setupUI();

    QLineEdit*      nameFilter_    = nullptr;
    QSpinBox*       minCallSpin_   = nullptr;
    QCheckBox*      hideLibCheck_  = nullptr;
    QSpinBox*       hopsSpin_      = nullptr;
    QPushButton*    clearFocusBtn_ = nullptr;
    QPushButton*    fitButton_     = nullptr;
    QPushButton*    resetButton_   = nullptr;
    QPushButton*    dotButton_     = nullptr;
    QLabel*         infoLabel_     = nullptr;
    QLabel*         focusLabel_    = nullptr;

    CallGraphScene* scene_         = nullptr;
    CallGraphView*  graphView_     = nullptr;
};

} // namespace retdec::gui::panels
#endif // RETDEC_GUI_PANELS_CALL_GRAPH_H
