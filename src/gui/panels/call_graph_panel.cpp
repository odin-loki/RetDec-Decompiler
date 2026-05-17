/**
 * @file src/gui/panels/call_graph_panel.cpp
 * @brief Interactive call-graph panel — full implementation (Task 52).
 *
 * Layout engine: force-directed spring embedding (Fruchterman-Reingold style).
 * SCC detection: iterative Tarjan.
 * Community backdrop: ModuleClusterItem at z = -2.
 */

#include "retdec/gui/panels/call_graph_panel.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyleOptionGraphicsItem>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QGraphicsSceneContextMenuEvent>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <random>
#include <stack>
#include <unordered_set>

namespace retdec::gui::panels {

// ─── Catppuccin Mocha ─────────────────────────────────────────────────────────
namespace clrCG {
static const QColor base     {0x1e, 0x1e, 0x2e};
static const QColor surface0 {0x31, 0x32, 0x44};
static const QColor surface1 {0x45, 0x47, 0x5a};
static const QColor surface2 {0x58, 0x5b, 0x70};
static const QColor overlay0 {0x6c, 0x70, 0x86};
static const QColor text     {0xcd, 0xd6, 0xf4};
static const QColor subtext0 {0xa6, 0xad, 0xc8};
static const QColor red      {0xf3, 0x8b, 0xa8};
static const QColor green    {0xa6, 0xe3, 0xa1};
static const QColor yellow   {0xf9, 0xe2, 0xaf};
static const QColor blue     {0x89, 0xb4, 0xfa};
static const QColor mauve    {0xcb, 0xa6, 0xf7};
static const QColor peach    {0xfa, 0xb3, 0x87};
static const QColor teal     {0x94, 0xe2, 0xd5};
static const QColor pink     {0xf5, 0xc2, 0xe7};
static const QColor flamingo {0xf2, 0xcd, 0xcd};
static const QColor rosewater{0xf5, 0xe0, 0xdc};
} // namespace clrCG

// ═════════════════════════════════════════════════════════════════════════════
// CallGraphNodeItem
// ═════════════════════════════════════════════════════════════════════════════

static constexpr qreal kMinNodeW = 100.0;
static constexpr qreal kMaxNodeW = 240.0;
static constexpr qreal kNodeH    =  34.0;

CallGraphNodeItem::CallGraphNodeItem(const CallGraphNode& node,
                                      QColor moduleColor,
                                      QGraphicsItem* parent)
    : QGraphicsObject(parent), node_(node), moduleColor_(moduleColor)
{
    setAcceptHoverEvents(true);
    setFlags(ItemIsSelectable);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QString("%1\n0x%2\n%3 instrs%4")
               .arg(node.name)
               .arg(node.address, 0, 16)
               .arg(node.instrCount)
               .arg(node.isLibrary ? "\n[library]" : ""));
}

qreal CallGraphNodeItem::nodeWidth() const
{
    // Scale with instruction count (log scale, clamped)
    if (node_.instrCount <= 0) return kMinNodeW;
    qreal scaled = kMinNodeW + std::log2(1.0 + node_.instrCount) * 10.0;
    return std::min(scaled, kMaxNodeW);
}

qreal CallGraphNodeItem::nodeHeight() const { return kNodeH; }

QRectF CallGraphNodeItem::nodeRect() const
{
    return QRectF(0, 0, nodeWidth(), nodeHeight());
}

QRectF CallGraphNodeItem::boundingRect() const
{
    return nodeRect().adjusted(-2, -2, 2, 2);
}

void CallGraphNodeItem::paint(QPainter* painter,
                               const QStyleOptionGraphicsItem*,
                               QWidget*)
{
    painter->setRenderHint(QPainter::Antialiasing);
    QRectF r = nodeRect();

    qreal alpha = dimmed_ ? 0.3 : 1.0;

    // Body
    QColor body = node_.isLibrary ? clrCG::surface0 : clrCG::surface1;
    body.setAlphaF(alpha);
    painter->setBrush(body);

    QColor border = hovered_    ? clrCG::blue
                  : isSelected() ? clrCG::blue
                  : node_.isLibrary ? clrCG::surface2 : moduleColor_;
    border.setAlphaF(alpha);
    painter->setPen(QPen(border, hovered_ ? 2.0 : 1.5));
    painter->drawRoundedRect(r, 5, 5);

    // Module colour stripe on left edge
    if (!node_.isLibrary) {
        QColor stripe = moduleColor_;
        stripe.setAlphaF(alpha * 0.8);
        painter->setBrush(stripe);
        painter->setPen(Qt::NoPen);
        QPainterPath sp;
        sp.addRoundedRect(QRectF(r.left(), r.top(), 4, r.height()), 5, 5);
        sp.addRect(QRectF(r.left() + 2, r.top(), 4, r.height()));
        painter->drawPath(sp.simplified());
    }

    // Function name
    painter->setFont(QFont("Cascadia Code,Consolas,Monospace", 8));
    QColor tc = node_.isLibrary ? clrCG::subtext0 : clrCG::text;
    tc.setAlphaF(alpha);
    painter->setPen(tc);
    QString displayName = node_.name;
    QFontMetrics fm(painter->font());
    displayName = fm.elidedText(displayName, Qt::ElideRight,
                                static_cast<int>(nodeWidth()) - 16);
    painter->drawText(r.adjusted(8, 0, -4, 0),
                      Qt::AlignVCenter | Qt::AlignLeft, displayName);

    // Instruction count badge (right side)
    if (node_.instrCount > 0) {
        painter->setFont(QFont("Monospace", 7));
        QColor nc = clrCG::overlay0;
        nc.setAlphaF(alpha);
        painter->setPen(nc);
        painter->drawText(r.adjusted(4, 0, -6, 0),
                          Qt::AlignVCenter | Qt::AlignRight,
                          QString::number(node_.instrCount));
    }
}

void CallGraphNodeItem::setDimmed(bool dim)
{
    if (dimmed_ == dim) return;
    dimmed_ = dim;
    update();
}

void CallGraphNodeItem::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    QGraphicsObject::mousePressEvent(event);
    emit clicked(node_.address);
}

void CallGraphNodeItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
    QMenu menu;
    menu.setStyleSheet(
        "QMenu { background: #313244; color: #cdd6f4; border: 1px solid #45475a; }"
        "QMenu::item:selected { background: #45475a; }");

    auto* focusAction1 = menu.addAction("Focus (1 hop)");
    auto* focusAction2 = menu.addAction("Focus (2 hops)");
    auto* focusAction3 = menu.addAction("Focus (3 hops)");
    menu.addSeparator();
    auto* navAction = menu.addAction("Navigate to Function");

    QAction* chosen = menu.exec(event->screenPos());
    if (!chosen) return;
    if (chosen == focusAction1) emit focusRequested(node_.address, 1);
    else if (chosen == focusAction2) emit focusRequested(node_.address, 2);
    else if (chosen == focusAction3) emit focusRequested(node_.address, 3);
    else if (chosen == navAction)    emit clicked(node_.address);
}

void CallGraphNodeItem::hoverEnterEvent(QGraphicsSceneHoverEvent* e)
{
    hovered_ = true;
    update();
    QGraphicsObject::hoverEnterEvent(e);
}

void CallGraphNodeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* e)
{
    hovered_ = false;
    update();
    QGraphicsObject::hoverLeaveEvent(e);
}

// ═════════════════════════════════════════════════════════════════════════════
// SccSuperNodeItem
// ═════════════════════════════════════════════════════════════════════════════

SccSuperNodeItem::SccSuperNodeItem(const std::vector<uint64_t>& members,
                                    const std::vector<QString>&   names,
                                    QGraphicsItem* parent)
    : QGraphicsObject(parent), members_(members), names_(names)
{
    setCursor(Qt::PointingHandCursor);
    setFlags(ItemIsSelectable);

    QString tip = QString("SCC: %1 mutually recursive functions\n").arg(members.size());
    for (size_t i = 0; i < std::min(names.size(), size_t{10}); ++i)
        tip += "  " + names[i] + "\n";
    if (names.size() > 10) tip += QString("  … +%1 more").arg(names.size() - 10);
    setToolTip(tip);
}

QRectF SccSuperNodeItem::boundingRect() const
{
    return QRectF(-2, -2, 124, 44);
}

void SccSuperNodeItem::paint(QPainter* painter,
                              const QStyleOptionGraphicsItem*,
                              QWidget*)
{
    painter->setRenderHint(QPainter::Antialiasing);
    QRectF r(0, 0, 120, 40);

    // Hatched fill
    QColor fillColor(0xfa, 0xb3, 0x87, 40);
    painter->setBrush(fillColor);
    painter->setPen(QPen(clrCG::peach, 1.8, Qt::DashLine));
    painter->drawRoundedRect(r, 6, 6);

    // SCC label
    painter->setFont(QFont("Monospace", 8, QFont::Bold));
    painter->setPen(clrCG::peach);
    painter->drawText(r, Qt::AlignCenter,
                      QString("SCC [%1]").arg(members_.size()));
    painter->setFont(QFont("Monospace", 7));
    painter->setPen(clrCG::subtext0);
    if (!names_.empty()) {
        QFontMetrics fm(painter->font());
        QString firstName = fm.elidedText(names_[0], Qt::ElideRight, 110);
        painter->drawText(r.adjusted(4, 18, -4, 0),
                          Qt::AlignTop | Qt::AlignHCenter, firstName);
    }
}

void SccSuperNodeItem::mousePressEvent(QGraphicsSceneMouseEvent*)
{
    emit clicked(members_);
}

// ═════════════════════════════════════════════════════════════════════════════
// CallGraphEdgeItem
// ═════════════════════════════════════════════════════════════════════════════

CallGraphEdgeItem::CallGraphEdgeItem(QPointF from, QPointF to,
                                      int callCount, bool isDirect,
                                      QGraphicsItem* parent)
    : QGraphicsPathItem(parent)
{
    buildPath(from, to, callCount, isDirect);
    setZValue(-1);
}

void CallGraphEdgeItem::buildPath(QPointF from, QPointF to,
                                   int callCount, bool isDirect)
{
    // Line width scales with call count (log scale, clamped 1–4)
    qreal width = 1.0 + std::log2(1.0 + callCount) * 0.4;
    width = std::min(width, 4.0);

    QColor color = isDirect ? QColor(0x89, 0xb4, 0xfa, 160)
                            : QColor(0xa6, 0xad, 0xc8, 120);
    QPen pen(color, width);
    pen.setCapStyle(Qt::RoundCap);
    if (!isDirect) {
        pen.setStyle(Qt::DashLine);
        pen.setDashPattern({5, 3});
    }
    setPen(pen);
    setBrush(Qt::NoBrush);

    QPainterPath path;
    path.moveTo(from);
    qreal cy = (from.y() + to.y()) / 2.0;
    path.cubicTo(from.x(), cy, to.x(), cy, to.x(), to.y());

    // Arrowhead
    addArrow(path, to, from);
    setPath(path);
}

void CallGraphEdgeItem::addArrow(QPainterPath& path, QPointF tip, QPointF incoming)
{
    QPointF dir = tip - incoming;
    qreal   len = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());
    if (len < 1e-6) return;
    dir /= len;
    QPointF perp(-dir.y(), dir.x());
    const qreal kAL = 9.0, kAW = 4.5;
    QPointF base = tip - dir * kAL;
    path.moveTo(tip);
    path.lineTo(base + perp * kAW);
    path.lineTo(base - perp * kAW);
    path.lineTo(tip);
}

// ═════════════════════════════════════════════════════════════════════════════
// ModuleClusterItem
// ═════════════════════════════════════════════════════════════════════════════

ModuleClusterItem::ModuleClusterItem(QRectF bounds, QColor color,
                                      const QString& label,
                                      QGraphicsItem* parent)
    : QGraphicsRectItem(bounds, parent), label_(label), color_(color)
{
    QColor fill = color;
    fill.setAlpha(18);
    setBrush(fill);
    QColor border = color;
    border.setAlpha(100);
    setPen(QPen(border, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    setZValue(-2);
}

void ModuleClusterItem::paint(QPainter* painter,
                               const QStyleOptionGraphicsItem* opt,
                               QWidget* w)
{
    QGraphicsRectItem::paint(painter, opt, w);

    if (label_.isEmpty()) return;
    QRectF r = rect();
    painter->setFont(QFont("Monospace", 8, QFont::Bold));
    QColor tc = color_;
    tc.setAlpha(180);
    painter->setPen(tc);
    painter->drawText(r.adjusted(6, 4, -6, 0),
                      Qt::AlignTop | Qt::AlignLeft, label_);
}

// ═════════════════════════════════════════════════════════════════════════════
// CallGraphView
// ═════════════════════════════════════════════════════════════════════════════

CallGraphView::CallGraphView(QGraphicsScene* scene, QWidget* parent)
    : QGraphicsView(scene, parent)
{
    setDragMode(NoDrag);
    setTransformationAnchor(AnchorUnderMouse);
    setResizeAnchor(AnchorViewCenter);
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    setBackgroundBrush(clrCG::base);
    setFrameShape(QFrame::NoFrame);
}

void CallGraphView::fitGraph()
{
    QRectF sr = scene()->sceneRect();
    if (!sr.isEmpty()) fitInView(sr.adjusted(-20,-20,20,20), Qt::KeepAspectRatio);
    emit viewportMoved();
}

void CallGraphView::resetZoom()
{
    resetTransform();
    emit viewportMoved();
}

void CallGraphView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        qreal f = event->angleDelta().y() > 0 ? 1.18 : 1.0 / 1.18;
        scale(f, f);
        emit viewportMoved();
        event->accept();
    } else {
        QGraphicsView::wheelEvent(event);
    }
}

void CallGraphView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        lastPan_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void CallGraphView::mouseMoveEvent(QMouseEvent* event)
{
    if (panning_) {
        QPoint d = event->pos() - lastPan_;
        lastPan_ = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        emit viewportMoved();
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void CallGraphView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        panning_ = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void CallGraphView::keyPressEvent(QKeyEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_F) { fitGraph();  return; }
        if (event->key() == Qt::Key_0) { resetZoom(); return; }
    }
    QGraphicsView::keyPressEvent(event);
}

void CallGraphView::scrollContentsBy(int dx, int dy)
{
    QGraphicsView::scrollContentsBy(dx, dy);
    emit viewportMoved();
}

// ═════════════════════════════════════════════════════════════════════════════
// CallGraphScene — layout engine
// ═════════════════════════════════════════════════════════════════════════════

CallGraphScene::CallGraphScene(QObject* parent)
    : QGraphicsScene(parent)
{
    setBackgroundBrush(clrCG::base);
}

QColor CallGraphScene::moduleColor(int moduleId)
{
    static const QColor kPalette[] = {
        QColor(0x89, 0xb4, 0xfa), // blue
        QColor(0xa6, 0xe3, 0xa1), // green
        QColor(0xcb, 0xa6, 0xf7), // mauve
        QColor(0xf9, 0xe2, 0xaf), // yellow
        QColor(0xfa, 0xb3, 0x87), // peach
        QColor(0x94, 0xe2, 0xd5), // teal
        QColor(0xf5, 0xc2, 0xe7), // pink
        QColor(0xf2, 0xcd, 0xcd), // flamingo
        QColor(0xf5, 0xe0, 0xdc), // rosewater
        QColor(0x6c, 0x70, 0x86), // overlay0
    };
    if (moduleId < 0) return QColor(0x58, 0x5b, 0x70);
    return kPalette[moduleId % static_cast<int>(sizeof(kPalette)/sizeof(kPalette[0]))];
}

// ── Tarjan iterative SCC ─────────────────────────────────────────────────────
CallGraphScene::SccResult CallGraphScene::computeSccs(
    const std::vector<uint64_t>& nodeAddrs,
    const std::unordered_map<uint64_t, std::vector<uint64_t>>& succ) const
{
    SccResult result;
    std::unordered_map<uint64_t, int> index, lowlink;
    std::unordered_set<uint64_t>      onStack;
    std::stack<uint64_t>              stk;
    int                               counter = 0;

    struct Frame {
        uint64_t node;
        int      succIdx;
    };

    for (uint64_t start : nodeAddrs) {
        if (index.count(start)) continue;

        std::stack<Frame> callStack;
        callStack.push({start, 0});
        index[start] = lowlink[start] = counter++;
        stk.push(start);
        onStack.insert(start);

        while (!callStack.empty()) {
            auto& [u, si] = callStack.top();
            auto succIt = succ.find(u);

            bool pushed = false;
            if (succIt != succ.end()) {
                while (si < static_cast<int>(succIt->second.size())) {
                    uint64_t v = succIt->second[static_cast<size_t>(si)];
                    ++si;
                    if (!index.count(v)) {
                        index[v] = lowlink[v] = counter++;
                        stk.push(v);
                        onStack.insert(v);
                        callStack.push({v, 0});
                        pushed = true;
                        break;
                    } else if (onStack.count(v)) {
                        lowlink[u] = std::min(lowlink[u], index[v]);
                    }
                }
            }

            if (!pushed) {
                if (lowlink[u] == index[u]) {
                    std::vector<uint64_t> scc;
                    uint64_t w;
                    do {
                        w = stk.top(); stk.pop();
                        onStack.erase(w);
                        scc.push_back(w);
                    } while (w != u);
                    result.sccs.push_back(std::move(scc));
                }
                callStack.pop();
                if (!callStack.empty()) {
                    auto& parent = callStack.top();
                    lowlink[parent.node] = std::min(lowlink[parent.node], lowlink[u]);
                }
            }
        }
    }
    return result;
}

// ── Force-directed layout (Fruchterman-Reingold) ─────────────────────────────
void CallGraphScene::forceDirectedLayout(
    std::vector<LayoutNode>&          nodes,
    const std::unordered_map<uint64_t, std::vector<uint64_t>>& edgeMap,
    int iterations) const
{
    if (nodes.empty()) return;

    const qreal area = 900.0 * 900.0 * std::sqrt(static_cast<qreal>(nodes.size()));
    const qreal k    = std::sqrt(area / static_cast<qreal>(nodes.size()));
    qreal temperature = std::sqrt(area) / 4.0;
    const qreal cooling = temperature / static_cast<qreal>(iterations);

    auto fa = [&](qreal d) { return (d * d) / k; };
    auto fr = [&](qreal d) { return (k * k) / std::max(d, 0.01); };

    std::unordered_map<uint64_t, int> idxOf;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
        idxOf[nodes[static_cast<size_t>(i)].addr] = i;

    for (int iter = 0; iter < iterations; ++iter) {
        // Reset forces
        for (auto& n : nodes) { n.fx = 0; n.fy = 0; }

        // Repulsive forces (all pairs, O(n^2) — acceptable for n<=1000)
        for (size_t i = 0; i < nodes.size(); ++i) {
            for (size_t j = i + 1; j < nodes.size(); ++j) {
                qreal dx = nodes[i].x - nodes[j].x;
                qreal dy = nodes[i].y - nodes[j].y;
                qreal d  = std::sqrt(dx * dx + dy * dy);
                if (d < 0.01) { d = 0.01; dx = 0.01; dy = 0.01; }
                qreal f  = fr(d) / d;
                nodes[i].fx += f * dx;
                nodes[i].fy += f * dy;
                nodes[j].fx -= f * dx;
                nodes[j].fy -= f * dy;
            }
        }

        // Attractive forces (edges)
        for (auto& [u, succs] : edgeMap) {
            auto uIt = idxOf.find(u);
            if (uIt == idxOf.end()) continue;
            for (uint64_t v : succs) {
                auto vIt = idxOf.find(v);
                if (vIt == idxOf.end()) continue;
                qreal dx = nodes[static_cast<size_t>(uIt->second)].x
                         - nodes[static_cast<size_t>(vIt->second)].x;
                qreal dy = nodes[static_cast<size_t>(uIt->second)].y
                         - nodes[static_cast<size_t>(vIt->second)].y;
                qreal d  = std::sqrt(dx * dx + dy * dy);
                if (d < 0.01) continue;
                qreal f  = fa(d) / d;
                nodes[static_cast<size_t>(uIt->second)].fx -= f * dx;
                nodes[static_cast<size_t>(uIt->second)].fy -= f * dy;
                nodes[static_cast<size_t>(vIt->second)].fx += f * dx;
                nodes[static_cast<size_t>(vIt->second)].fy += f * dy;
            }
        }

        // Apply forces with temperature limit
        for (auto& n : nodes) {
            qreal fm = std::sqrt(n.fx * n.fx + n.fy * n.fy);
            if (fm > 0) {
                qreal scale = std::min(fm, temperature) / fm;
                n.x += n.fx * scale;
                n.y += n.fy * scale;
            }
        }
        temperature -= cooling;
    }
}

// ── Main loadGraph ────────────────────────────────────────────────────────────
void CallGraphScene::loadGraph(const std::vector<CallGraphNode>& nodes,
                                const std::vector<CallEdge>&      edges)
{
    clearGraph();
    nodes_ = nodes;
    edges_ = edges;

    if (nodes.empty()) return;

    // Limit display to kMaxNodes
    std::vector<CallGraphNode> visNodes = nodes;
    if (static_cast<int>(visNodes.size()) > kMaxNodes) {
        visNodes.resize(static_cast<size_t>(kMaxNodes));
    }

    // Build adjacency (succ and pred)
    std::unordered_map<uint64_t, std::vector<uint64_t>> succ;
    std::unordered_set<uint64_t>                         nodeSet;
    for (auto& n : visNodes) nodeSet.insert(n.address);
    for (auto& e : edges) {
        if (nodeSet.count(e.callerAddress) && nodeSet.count(e.calleeAddress)) {
            succ[e.callerAddress].push_back(e.calleeAddress);
        }
    }

    // Addresses list
    std::vector<uint64_t> addrs;
    addrs.reserve(visNodes.size());
    for (auto& n : visNodes) addrs.push_back(n.address);

    // ── SCC detection ─────────────────────────────────────────────────────
    auto sccResult = computeSccs(addrs, succ);
    std::unordered_map<uint64_t, int> sccId; // addr → scc index
    for (size_t si = 0; si < sccResult.sccs.size(); ++si) {
        for (uint64_t a : sccResult.sccs[si])
            sccId[a] = static_cast<int>(si);
    }

    // Nodes in large SCCs will be represented by a super-node
    std::unordered_set<uint64_t> inLargeScc;
    std::unordered_map<int, std::vector<size_t>> sccToVisNodeIdx;
    for (size_t si = 0; si < sccResult.sccs.size(); ++si) {
        if (sccResult.sccs[si].size() > 1) {
            for (uint64_t a : sccResult.sccs[si]) {
                inLargeScc.insert(a);
            }
        }
    }
    for (size_t i = 0; i < visNodes.size(); ++i) {
        int sid = sccId.count(visNodes[i].address) ? sccId[visNodes[i].address] : -1;
        if (sid >= 0 && sccResult.sccs[static_cast<size_t>(sid)].size() > 1) {
            sccToVisNodeIdx[sid].push_back(i);
        }
    }

    // ── Build layout nodes ─────────────────────────────────────────────────
    // Map from address (or scc representative address) → LayoutNode
    std::unordered_map<uint64_t, std::vector<uint64_t>> layoutEdges;
    std::vector<LayoutNode> layoutNodes;
    std::unordered_map<uint64_t, uint64_t> sccRepresentative; // member→rep

    // Add non-SCC nodes
    std::mt19937 rng(42);
    std::uniform_real_distribution<qreal> rnd(-200, 200);

    for (auto& n : visNodes) {
        if (inLargeScc.count(n.address)) continue;
        LayoutNode ln;
        ln.addr = n.address;
        ln.x    = rnd(rng);
        ln.y    = rnd(rng);
        layoutNodes.push_back(ln);
    }

    // Add SCC super-nodes (one per large SCC)
    std::unordered_set<int> addedScc;
    for (auto& [sid, indices] : sccToVisNodeIdx) {
        if (addedScc.count(sid)) continue;
        addedScc.insert(sid);
        uint64_t rep = visNodes[indices[0]].address;
        for (uint64_t a : sccResult.sccs[static_cast<size_t>(sid)])
            sccRepresentative[a] = rep;
        LayoutNode ln;
        ln.addr = rep;
        ln.x    = rnd(rng);
        ln.y    = rnd(rng);
        layoutNodes.push_back(ln);
    }

    // Build layout edge map (using representative addresses)
    auto resolve = [&](uint64_t a) -> uint64_t {
        auto it = sccRepresentative.find(a);
        return it != sccRepresentative.end() ? it->second : a;
    };
    for (auto& e : edges) {
        uint64_t u = resolve(e.callerAddress);
        uint64_t v = resolve(e.calleeAddress);
        if (u != v && nodeSet.count(e.callerAddress) && nodeSet.count(e.calleeAddress))
            layoutEdges[u].push_back(v);
    }

    // ── Force-directed layout ─────────────────────────────────────────────
    forceDirectedLayout(layoutNodes, layoutEdges,
                        layoutNodes.size() > 300 ? 20 : 60);

    // Normalise coordinates
    qreal minX = std::numeric_limits<qreal>::max();
    qreal minY = std::numeric_limits<qreal>::max();
    for (auto& ln : layoutNodes) { minX = std::min(minX, ln.x); minY = std::min(minY, ln.y); }
    for (auto& ln : layoutNodes) { ln.x -= minX - kMargin; ln.y -= minY - kMargin; }

    std::unordered_map<uint64_t, QPointF> positions;
    for (auto& ln : layoutNodes) positions[ln.addr] = QPointF(ln.x, ln.y);

    // ── Create node items ─────────────────────────────────────────────────
    std::unordered_map<uint64_t, const CallGraphNode*> nodeByAddr;
    for (auto& n : visNodes) nodeByAddr[n.address] = &n;

    // Regular nodes
    for (auto& n : visNodes) {
        if (inLargeScc.count(n.address)) continue;
        QPointF pos = positions.count(n.address) ? positions[n.address] : QPointF(0,0);
        auto* item = new CallGraphNodeItem(n, moduleColor(n.moduleId));
        item->setPos(pos);
        addItem(item);
        nodeItems_[n.address] = item;
        connect(item, &CallGraphNodeItem::clicked, this, &CallGraphScene::nodeClicked);
        connect(item, &CallGraphNodeItem::focusRequested, this, &CallGraphScene::focusRequested);
    }

    // SCC super-nodes
    for (auto& [sid, indices] : sccToVisNodeIdx) {
        if (!addedScc.count(sid)) continue; // shouldn't happen
        uint64_t rep = visNodes[indices[0]].address;
        std::vector<uint64_t> members;
        std::vector<QString>  names;
        for (uint64_t a : sccResult.sccs[static_cast<size_t>(sid)]) {
            members.push_back(a);
            auto it = nodeByAddr.find(a);
            if (it != nodeByAddr.end()) names.push_back(it->second->name);
        }
        QPointF pos = positions.count(rep) ? positions[rep] : QPointF(0,0);
        auto* sccItem = new SccSuperNodeItem(members, names);
        sccItem->setPos(pos);
        addItem(sccItem);
        sccItems_[rep] = sccItem;
    }

    // ── Create edges ──────────────────────────────────────────────────────
    // Count calls per edge (from the raw edges)
    std::unordered_map<uint64_t, int> callCountMap;
    auto edgeKey = [](uint64_t a, uint64_t b) {
        return (a << 32) ^ b;
    };
    for (auto& e : edges) {
        uint64_t u = resolve(e.callerAddress);
        uint64_t v = resolve(e.calleeAddress);
        if (u == v) continue;
        callCountMap[edgeKey(u,v)] += e.callCount;
    }

    std::unordered_set<uint64_t> drawnEdges;
    for (auto& e : edges) {
        uint64_t u = resolve(e.callerAddress);
        uint64_t v = resolve(e.calleeAddress);
        if (u == v) continue;
        uint64_t key = edgeKey(u, v);
        if (drawnEdges.count(key)) continue;
        drawnEdges.insert(key);

        // Find from/to items
        QPointF from, to;
        bool hasBoth = false;
        if (nodeItems_.count(u) && nodeItems_.count(v)) {
            auto* fi = nodeItems_[u];
            auto* ti = nodeItems_[v];
            QRectF fr = fi->mapToScene(fi->nodeRect()).boundingRect();
            QRectF tr = ti->mapToScene(ti->nodeRect()).boundingRect();
            from = fr.center();
            from.setY(fr.bottom());
            to   = tr.center();
            to.setY(tr.top());
            hasBoth = true;
        } else if (sccItems_.count(u) && nodeItems_.count(v)) {
            from = sccItems_[u]->mapToScene(sccItems_[u]->boundingRect().center());
            auto* ti = nodeItems_[v];
            QRectF tr = ti->mapToScene(ti->nodeRect()).boundingRect();
            to = tr.center();
            hasBoth = true;
        } else if (nodeItems_.count(u) && sccItems_.count(v)) {
            auto* fi = nodeItems_[u];
            QRectF fr = fi->mapToScene(fi->nodeRect()).boundingRect();
            from = fr.center();
            to   = sccItems_[v]->mapToScene(sccItems_[v]->boundingRect().center());
            hasBoth = true;
        }
        if (hasBoth) {
            int count = callCountMap.count(key) ? callCountMap[key] : 1;
            addItem(new CallGraphEdgeItem(from, to, count, e.isDirect));
        }
    }

    // ── Module cluster backdrops ───────────────────────────────────────────
    std::unordered_map<int, QRectF> moduleRects;
    for (auto& n : visNodes) {
        if (n.moduleId < 0 || inLargeScc.count(n.address)) continue;
        auto it = nodeItems_.find(n.address);
        if (it == nodeItems_.end()) continue;
        QRectF r = it->second->mapToScene(it->second->nodeRect()).boundingRect();
        if (!moduleRects.count(n.moduleId)) {
            moduleRects[n.moduleId] = r;
        } else {
            moduleRects[n.moduleId] = moduleRects[n.moduleId].united(r);
        }
    }
    for (auto& [mid, rect] : moduleRects) {
        rect.adjust(-15, -20, 15, 15);
        QString label = QString("Module %1").arg(mid);
        addItem(new ModuleClusterItem(rect, moduleColor(mid), label));
    }

    setSceneRect(itemsBoundingRect().adjusted(-kMargin, -kMargin, kMargin, kMargin));
}

void CallGraphScene::clearGraph()
{
    clear();
    nodeItems_.clear();
    sccItems_.clear();
    focusAddress_ = 0;
    focusSet_.clear();
}

int CallGraphScene::visibleNodeCount() const
{
    int count = 0;
    for (auto& [addr, item] : nodeItems_) {
        if (!item->isVisible()) continue;
        ++count;
    }
    return count;
}

void CallGraphScene::setNameFilter(const QString& glob)
{
    nameFilter_ = glob;
    applyFilters();
}

void CallGraphScene::setMinCallCount(int minCount)
{
    minCallCount_ = minCount;
    applyFilters();
}

void CallGraphScene::setHideLibrary(bool hide)
{
    hideLibrary_ = hide;
    applyFilters();
}

void CallGraphScene::setModuleFilter(int moduleId)
{
    moduleFilter_ = moduleId;
    applyFilters();
}

void CallGraphScene::setFocusNode(uint64_t address, int hops)
{
    focusAddress_ = address;
    focusHops_    = hops;
    focusSet_.clear();

    // BFS in both directions up to 'hops' levels
    std::unordered_map<uint64_t, std::vector<uint64_t>> succ, pred;
    for (auto& e : edges_) {
        succ[e.callerAddress].push_back(e.calleeAddress);
        pred[e.calleeAddress].push_back(e.callerAddress);
    }

    std::deque<std::pair<uint64_t, int>> queue;
    queue.push_back({address, 0});
    focusSet_.insert(address);

    while (!queue.empty()) {
        auto [node, depth] = queue.front();
        queue.pop_front();
        if (depth >= hops) continue;
        for (uint64_t n : succ[node]) {
            if (!focusSet_.count(n)) { focusSet_.insert(n); queue.push_back({n, depth+1}); }
        }
        for (uint64_t n : pred[node]) {
            if (!focusSet_.count(n)) { focusSet_.insert(n); queue.push_back({n, depth+1}); }
        }
    }

    applyFilters();
}

void CallGraphScene::clearFocus()
{
    focusAddress_ = 0;
    focusSet_.clear();
    applyFilters();
}

void CallGraphScene::applyFilters()
{
    for (auto& [addr, item] : nodeItems_) {
        bool show = isNodeVisible(addr);
        item->setVisible(show);
        item->setDimmed(!focusSet_.empty() && !focusSet_.count(addr));
    }
}

bool CallGraphScene::isNodeVisible(uint64_t addr) const
{
    // Focus filter
    if (!focusSet_.empty() && !focusSet_.count(addr)) return false;

    // Find node info
    const CallGraphNode* node = nullptr;
    for (auto& n : nodes_) {
        if (n.address == addr) { node = &n; break; }
    }
    if (!node) return true;

    if (hideLibrary_ && node->isLibrary) return false;
    if (moduleFilter_ >= 0 && node->moduleId != moduleFilter_) return false;
    if (!nameFilter_.isEmpty()) {
        // Glob: * matches anything
        QRegularExpression re(
            QRegularExpression::wildcardToRegularExpression(nameFilter_),
            QRegularExpression::CaseInsensitiveOption);
        if (!re.match(node->name).hasMatch()) return false;
    }
    return true;
}

QString CallGraphScene::exportDot() const
{
    QString dot = "digraph callgraph {\n"
                  "  graph [bgcolor=\"#1e1e2e\" fontcolor=\"#cdd6f4\"];\n"
                  "  node [shape=box style=filled fillcolor=\"#313244\""
                  " color=\"#45475a\" fontcolor=\"#cdd6f4\" fontname=\"Consolas\"];\n"
                  "  edge [color=\"#89b4fa\" fontcolor=\"#cdd6f4\"];\n\n";

    for (auto& n : nodes_) {
        dot += QString("  \"%1\" [label=\"%2\\n0x%3\"%4];\n")
               .arg(n.address)
               .arg(n.name.toHtmlEscaped())
               .arg(n.address, 0, 16)
               .arg(n.isLibrary ? " style=dashed" : "");
    }

    dot += "\n";

    // Deduplicate edges
    std::unordered_set<uint64_t> seen;
    for (auto& e : edges_) {
        uint64_t key = (e.callerAddress << 32) ^ e.calleeAddress;
        if (seen.count(key)) continue;
        seen.insert(key);
        dot += QString("  \"%1\" -> \"%2\" [label=\"%3\"%4];\n")
               .arg(e.callerAddress)
               .arg(e.calleeAddress)
               .arg(e.callCount)
               .arg(e.isDirect ? "" : " style=dashed");
    }

    dot += "}\n";
    return dot;
}

// ═════════════════════════════════════════════════════════════════════════════
// CallGraphPanel
// ═════════════════════════════════════════════════════════════════════════════

CallGraphPanel::CallGraphPanel(QWidget* parent)
    : PanelBase("Call Graph", parent)
{
    setupUI();
}

void CallGraphPanel::setupUI()
{
    // ── Toolbar ──────────────────────────────────────────────────────────
    auto* toolbar    = new QWidget(this);
    auto* tbLayout   = new QHBoxLayout(toolbar);
    tbLayout->setContentsMargins(4, 3, 4, 3);
    tbLayout->setSpacing(4);

    auto styleEdit = [](QWidget* w) {
        w->setStyleSheet(
            "background: #313244; color: #cdd6f4; border: 1px solid #45475a;"
            " border-radius: 4px; padding: 2px 4px;");
    };
    auto styleBtn = [](QPushButton* b) {
        b->setFixedHeight(24);
        b->setStyleSheet(
            "QPushButton { background: #313244; color: #cdd6f4;"
            " border: 1px solid #45475a; border-radius: 4px; padding: 0 8px; }"
            "QPushButton:hover { background: #45475a; }"
            "QPushButton:pressed { background: #585b70; }");
    };

    nameFilter_ = new QLineEdit(toolbar);
    nameFilter_->setPlaceholderText("Filter functions…");
    nameFilter_->setClearButtonEnabled(true);
    nameFilter_->setFixedWidth(160);
    styleEdit(nameFilter_);

    auto* minCallLabel = new QLabel("Min calls:", toolbar);
    minCallLabel->setStyleSheet("color: #a6adc8; font-size: 11px;");
    minCallSpin_ = new QSpinBox(toolbar);
    minCallSpin_->setRange(0, 100000);
    minCallSpin_->setValue(0);
    minCallSpin_->setFixedWidth(60);
    styleEdit(minCallSpin_);

    hideLibCheck_ = new QCheckBox("Hide lib", toolbar);
    hideLibCheck_->setStyleSheet("color: #cdd6f4; font-size: 11px;");

    auto* hopsLabel = new QLabel("Focus hops:", toolbar);
    hopsLabel->setStyleSheet("color: #a6adc8; font-size: 11px;");
    hopsSpin_ = new QSpinBox(toolbar);
    hopsSpin_->setRange(1, 10);
    hopsSpin_->setValue(2);
    hopsSpin_->setFixedWidth(40);
    styleEdit(hopsSpin_);

    clearFocusBtn_ = new QPushButton("Clear Focus", toolbar);
    styleBtn(clearFocusBtn_);
    clearFocusBtn_->setEnabled(false);

    fitButton_   = new QPushButton("Fit",        toolbar);
    resetButton_ = new QPushButton("100%",       toolbar);
    dotButton_   = new QPushButton("Export DOT", toolbar);
    for (auto* b : {fitButton_, resetButton_, dotButton_}) styleBtn(b);

    infoLabel_  = new QLabel("", toolbar);
    infoLabel_->setStyleSheet("color: #a6adc8; font-size: 11px;");
    focusLabel_ = new QLabel("", toolbar);
    focusLabel_->setStyleSheet("color: #89b4fa; font-size: 11px;");

    tbLayout->addWidget(nameFilter_);
    tbLayout->addWidget(minCallLabel);
    tbLayout->addWidget(minCallSpin_);
    tbLayout->addWidget(hideLibCheck_);
    tbLayout->addWidget(hopsLabel);
    tbLayout->addWidget(hopsSpin_);
    tbLayout->addWidget(clearFocusBtn_);
    tbLayout->addStretch(1);
    tbLayout->addWidget(focusLabel_);
    tbLayout->addWidget(infoLabel_);
    tbLayout->addWidget(fitButton_);
    tbLayout->addWidget(resetButton_);
    tbLayout->addWidget(dotButton_);

    // ── Scene & view ─────────────────────────────────────────────────────
    scene_     = new CallGraphScene(this);
    graphView_ = new CallGraphView(scene_, this);

    // ── Layout ────────────────────────────────────────────────────────────
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(toolbar);
    layout->addWidget(graphView_);

    // ── Connections ───────────────────────────────────────────────────────
    connect(fitButton_,   &QPushButton::clicked, this, &CallGraphPanel::onFitView);
    connect(resetButton_, &QPushButton::clicked, this, &CallGraphPanel::onResetZoom);
    connect(dotButton_,   &QPushButton::clicked, this, &CallGraphPanel::onExportDot);
    connect(clearFocusBtn_, &QPushButton::clicked, this, &CallGraphPanel::onClearFocus);
    connect(nameFilter_,  &QLineEdit::textChanged, this, &CallGraphPanel::onNameFilterChanged);
    connect(minCallSpin_,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this, &CallGraphPanel::onMinCallCountChanged);
    connect(hideLibCheck_, &QCheckBox::toggled, this, &CallGraphPanel::onHideLibraryToggled);
    connect(scene_, &CallGraphScene::nodeClicked, this, &CallGraphPanel::onNodeClicked);
    connect(scene_, &CallGraphScene::focusRequested, this, &CallGraphPanel::onFocusRequested);
}

void CallGraphPanel::loadGraph(const std::vector<CallGraphNode>& nodes,
                                const std::vector<CallEdge>&      edges)
{
    scene_->loadGraph(nodes, edges);
    infoLabel_->setText(QString("%1 nodes").arg(nodes.size()));
    graphView_->fitGraph();
}

void CallGraphPanel::setCallGraph(const QStringList& functionNames,
                                   const std::vector<CallEdge>& edges)
{
    std::vector<CallGraphNode> nodes;
    nodes.reserve(static_cast<size_t>(functionNames.size()));
    for (int i = 0; i < functionNames.size(); ++i) {
        CallGraphNode n;
        n.address = static_cast<uint64_t>(i);
        n.name    = functionNames[i];
        nodes.push_back(n);
    }
    loadGraph(nodes, edges);
}

void CallGraphPanel::clear()
{
    scene_->clearGraph();
    nameFilter_->clear();
    minCallSpin_->setValue(0);
    hideLibCheck_->setChecked(false);
    infoLabel_->setText("");
    focusLabel_->setText("");
    clearFocusBtn_->setEnabled(false);
}

void CallGraphPanel::onFunctionSelected(uint64_t /*address*/, const QString& name)
{
    focusLabel_->setText(name);
}

void CallGraphPanel::onFitView()    { graphView_->fitGraph();  }
void CallGraphPanel::onResetZoom()  { graphView_->resetZoom(); }

void CallGraphPanel::onExportDot()
{
    QString path = QFileDialog::getSaveFileName(
        this, "Export Call Graph (DOT)", QString(), "Graphviz DOT (*.dot *.gv)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Failed",
                             "Cannot write to " + path);
        return;
    }
    file.write(scene_->exportDot().toUtf8());
}

void CallGraphPanel::onNameFilterChanged(const QString& text)
{
    scene_->setNameFilter(text);
    infoLabel_->setText(QString("%1 visible").arg(scene_->visibleNodeCount()));
}

void CallGraphPanel::onMinCallCountChanged(int value)
{
    scene_->setMinCallCount(value);
    infoLabel_->setText(QString("%1 visible").arg(scene_->visibleNodeCount()));
}

void CallGraphPanel::onHideLibraryToggled(bool hide)
{
    scene_->setHideLibrary(hide);
    infoLabel_->setText(QString("%1 visible").arg(scene_->visibleNodeCount()));
}

void CallGraphPanel::onNodeClicked(uint64_t address)
{
    emit functionNavigationRequested(address);
}

void CallGraphPanel::onFocusRequested(uint64_t address, int /*hops*/)
{
    int hops = hopsSpin_->value();
    scene_->setFocusNode(address, hops);
    focusLabel_->setText(QString("Focus: 0x%1 (%2 hops)").arg(address, 0, 16).arg(hops));
    clearFocusBtn_->setEnabled(true);
    infoLabel_->setText(QString("%1 visible").arg(scene_->visibleNodeCount()));
}

void CallGraphPanel::onClearFocus()
{
    scene_->clearFocus();
    focusLabel_->setText("");
    clearFocusBtn_->setEnabled(false);
    infoLabel_->setText(QString("%1 visible").arg(scene_->visibleNodeCount()));
}

void CallGraphPanel::onDepthChanged(int /*depth*/) {}

} // namespace retdec::gui::panels
