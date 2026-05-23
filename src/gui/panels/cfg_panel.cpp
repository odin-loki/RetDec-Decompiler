/**
 * @file src/gui/panels/cfg_panel.cpp
 * @brief Interactive CFG visualiser — full implementation (Task 51).
 */

#include "retdec/gui/panels/cfg_panel.h"

#include "retdec/gui/widgets/empty_state_widget.h"

#include <QApplication>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyleOptionGraphicsItem>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <stack>

namespace retdec::gui::panels {

// ─── Catppuccin Mocha palette ─────────────────────────────────────────────────
namespace clr {
static const QColor base     {0x1e, 0x1e, 0x2e};
static const QColor mantle   {0x18, 0x18, 0x25};
static const QColor crust    {0x11, 0x11, 0x1b};
static const QColor surface0 {0x31, 0x32, 0x44};
static const QColor surface1 {0x45, 0x47, 0x5a};
static const QColor surface2 {0x58, 0x5b, 0x70};
static const QColor overlay0 {0x6c, 0x70, 0x86};
static const QColor overlay2 {0x9a, 0x9e, 0xbc};
static const QColor text     {0xcd, 0xd6, 0xf4};
static const QColor subtext0 {0xa6, 0xad, 0xc8};
static const QColor subtext1 {0xba, 0xc2, 0xde};
static const QColor red      {0xf3, 0x8b, 0xa8};
static const QColor green    {0xa6, 0xe3, 0xa1};
static const QColor yellow   {0xf9, 0xe2, 0xaf};
static const QColor blue     {0x89, 0xb4, 0xfa};
static const QColor mauve    {0xcb, 0xa6, 0xf7};
static const QColor peach    {0xfa, 0xb3, 0x87};
static const QColor teal     {0x94, 0xe2, 0xd5};
} // namespace clr

// ═════════════════════════════════════════════════════════════════════════════
// BasicBlockItem
// ═════════════════════════════════════════════════════════════════════════════

BasicBlockItem::BasicBlockItem(const BasicBlockData& data, QGraphicsItem* parent)
    : QGraphicsObject(parent), data_(data)
{
    setAcceptHoverEvents(true);
    setFlags(ItemIsSelectable);
    setCursor(Qt::PointingHandCursor);
}

qreal BasicBlockItem::totalHeight() const
{
    int preview = std::min(static_cast<int>(data_.instrs.size()), kMaxPreview);
    bool truncated = static_cast<int>(data_.instrs.size()) > kMaxPreview;
    return kHeaderH + preview * kInstrH + (truncated ? kInstrH : 0) + kFooterH;
}

QRectF BasicBlockItem::boundingRect() const
{
    return QRectF(0, 0, kWidth, totalHeight());
}

QPointF BasicBlockItem::bottomCentre() const
{
    return mapToScene(QPointF(kWidth / 2, totalHeight()));
}

QPointF BasicBlockItem::topCentre() const
{
    return mapToScene(QPointF(kWidth / 2, 0));
}

void BasicBlockItem::paint(QPainter* painter,
                            const QStyleOptionGraphicsItem*,
                            QWidget*)
{
    painter->setRenderHint(QPainter::Antialiasing);

    qreal h = totalHeight();
    QRectF rect(0, 0, kWidth, h);

    // ── Shadow ──────────────────────────────────────────────────────────────
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 50));
    painter->drawRoundedRect(rect.adjusted(3, 3, 3, 3), kRadius, kRadius);

    // ── Body ────────────────────────────────────────────────────────────────
    QColor bodyColor = hovered_      ? clr::surface1
                     : isSelected()  ? clr::surface2
                     : clr::surface0;
    painter->setBrush(bodyColor);
    QColor borderColor = data_.isLoopHeader ? clr::blue
                       : data_.isEntry      ? clr::green
                       : data_.isExit       ? clr::red
                       : hovered_           ? clr::overlay2
                       : clr::surface1;
    painter->setPen(QPen(borderColor, hovered_ ? 2.0 : 1.5));
    painter->drawRoundedRect(rect, kRadius, kRadius);

    // ── Header stripe (address) ──────────────────────────────────────────
    QRectF headerRect(0, 0, kWidth, kHeaderH);
    QColor headerBg = data_.isLoopHeader ? QColor(clr::blue.red(), clr::blue.green(), clr::blue.blue(), 80)
                    : data_.isEntry      ? QColor(clr::green.red(), clr::green.green(), clr::green.blue(), 60)
                    : data_.isExit       ? QColor(clr::red.red(), clr::red.green(), clr::red.blue(), 60)
                    : clr::mantle;
    painter->setPen(Qt::NoPen);
    painter->setBrush(headerBg);
    // Only round top corners
    QPainterPath headerPath;
    headerPath.addRoundedRect(headerRect, kRadius, kRadius);
    QRectF bottom = headerRect.adjusted(0, kRadius, 0, 0);
    headerPath.addRect(bottom);
    painter->drawPath(headerPath.simplified());

    // Header text
    painter->setFont(QFont("Cascadia Code,Consolas,Monospace", 8, QFont::Bold));
    painter->setPen(clr::subtext1);
    QString addrText = QString("0x%1").arg(data_.address, 16, 16, QChar('0'));
    if (data_.isEntry)       addrText += "  [entry]";
    else if (data_.isExit)   addrText += "  [exit]";
    else if (data_.isLoopHeader) addrText += "  [loop header]";
    painter->drawText(QRectF(kPadX, 0, kWidth - kPadX * 2, kHeaderH),
                      Qt::AlignVCenter | Qt::AlignLeft, addrText);

    // ── Instructions ─────────────────────────────────────────────────────
    painter->setFont(QFont("Cascadia Code,Consolas,Monospace", 8));
    int shown = std::min(static_cast<int>(data_.instrs.size()), kMaxPreview);
    for (int i = 0; i < shown; ++i) {
        qreal iy = kHeaderH + i * kInstrH;
        painter->setPen(clr::text);
        QString line = data_.instrs[static_cast<size_t>(i)].text;
        painter->drawText(QRectF(kPadX, iy, kWidth - kPadX * 2, kInstrH),
                          Qt::AlignVCenter | Qt::AlignLeft, line);
    }
    if (static_cast<int>(data_.instrs.size()) > kMaxPreview) {
        qreal iy = kHeaderH + shown * kInstrH;
        painter->setPen(clr::overlay0);
        int extra = static_cast<int>(data_.instrs.size()) - kMaxPreview;
        painter->drawText(QRectF(kPadX, iy, kWidth - kPadX * 2, kInstrH),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          QString("  … %1 more").arg(extra));
    }

    // ── Selection glow ────────────────────────────────────────────────────
    if (isSelected()) {
        painter->setPen(QPen(clr::blue, 2.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(rect, kRadius, kRadius);
    }
}

void BasicBlockItem::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    QGraphicsObject::mousePressEvent(event);
    emit clicked(data_.id);
}

void BasicBlockItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = true;
    update();
    QGraphicsObject::hoverEnterEvent(event);
}

void BasicBlockItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event)
{
    hovered_ = false;
    update();
    QGraphicsObject::hoverLeaveEvent(event);
}

// ═════════════════════════════════════════════════════════════════════════════
// CFGEdgeItem
// ═════════════════════════════════════════════════════════════════════════════

QColor CFGEdgeItem::colorForKind(EdgeKind kind)
{
    switch (kind) {
    case EdgeKind::FallThrough:        return clr::subtext0;
    case EdgeKind::TrueBranch:         return clr::green;
    case EdgeKind::FalseBranch:        return clr::red;
    case EdgeKind::BackEdge:           return clr::peach;
    case EdgeKind::ExceptionEdge:      return clr::mauve;
    case EdgeKind::UnresolvedIndirect: return clr::red;
    }
    return clr::subtext0;
}

CFGEdgeItem::CFGEdgeItem(QPointF from, QPointF to, EdgeKind kind,
                          bool isCurvedBack, QGraphicsItem* parent)
    : QGraphicsPathItem(parent)
{
    buildPath(from, to, kind, isCurvedBack);
    setZValue(-1);
}

void CFGEdgeItem::buildPath(QPointF from, QPointF to, EdgeKind kind, bool isCurvedBack)
{
    QColor color = colorForKind(kind);

    QPen pen(color, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    if (kind == EdgeKind::BackEdge || kind == EdgeKind::UnresolvedIndirect) {
        pen.setStyle(Qt::DashLine);
        pen.setDashPattern({4, 3});
    }
    setPen(pen);
    setBrush(Qt::NoBrush);

    QPainterPath path;
    path.moveTo(from);

    if (isCurvedBack) {
        // Arc along the left side to avoid crossing forward edges
        qreal leftX  = std::min(from.x(), to.x()) - 80.0;
        qreal midY   = (from.y() + to.y()) / 2.0;
        path.cubicTo(QPointF(leftX, from.y()),
                     QPointF(leftX, to.y()),
                     QPointF(to.x(), to.y()));
    } else {
        qreal cy = (from.y() + to.y()) / 2.0;
        path.cubicTo(QPointF(from.x(), cy),
                     QPointF(to.x(),   cy),
                     QPointF(to.x(),   to.y()));
    }

    addArrowHead(path, to, from);
    setPath(path);
}

void CFGEdgeItem::addArrowHead(QPainterPath& path, QPointF tip, QPointF incoming)
{
    // Direction vector (from incoming toward tip)
    QPointF dir = tip - incoming;
    qreal   len = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());
    if (len < 1e-6) return;
    dir /= len;

    const qreal kAL = 10.0; // arrowhead length
    const qreal kAW =  5.0; // arrowhead half-width

    QPointF perp(-dir.y(), dir.x());
    QPointF base = tip - dir * kAL;

    path.moveTo(tip);
    path.lineTo(base + perp * kAW);
    path.lineTo(base - perp * kAW);
    path.lineTo(tip);
}

// ═════════════════════════════════════════════════════════════════════════════
// LoopRegionItem
// ═════════════════════════════════════════════════════════════════════════════

LoopRegionItem::LoopRegionItem(QRectF bounds, QColor color, QGraphicsItem* parent)
    : QGraphicsRectItem(bounds, parent)
{
    QColor fill = color;
    fill.setAlpha(25);
    QColor border = color;
    border.setAlpha(140);
    setBrush(fill);
    setPen(QPen(border, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    setZValue(-2);
}

// ═════════════════════════════════════════════════════════════════════════════
// MiniMapView
// ═════════════════════════════════════════════════════════════════════════════

MiniMapView::MiniMapView(QGraphicsScene* scene, QGraphicsView* mainView,
                          QWidget* parent)
    : QGraphicsView(scene, parent), mainView_(mainView)
{
    setFixedSize(160, 120);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setInteractive(false);
    setRenderHint(QPainter::Antialiasing);

    // Dark translucent background
    setStyleSheet("background: rgba(24,24,37,210); border: 1px solid #45475a;"
                  "border-radius: 4px;");
}

void MiniMapView::syncViewport()
{
    if (!mainView_ || !scene()) return;

    // Scale minimap to show the full scene
    QRectF sr = scene()->sceneRect();
    if (sr.isEmpty()) return;
    fitInView(sr, Qt::KeepAspectRatio);

    update();
}

void MiniMapView::paintEvent(QPaintEvent* event)
{
    QGraphicsView::paintEvent(event);

    if (!mainView_) return;

    // Draw viewport indicator
    QRectF vp = mainView_->mapToScene(mainView_->viewport()->rect()).boundingRect();
    QPointF topLeft     = mapFromScene(vp.topLeft());
    QPointF bottomRight = mapFromScene(vp.bottomRight());
    QRectF  vpRect(topLeft, bottomRight);

    QPainter p(viewport());
    p.setPen(QPen(QColor(0x89, 0xb4, 0xfa, 200), 1.5));
    p.setBrush(QColor(0x89, 0xb4, 0xfa, 40));
    p.drawRect(vpRect);
}

void MiniMapView::mousePressEvent(QMouseEvent* event)
{
    dragging_ = true;
    panMainViewTo(event->pos());
}

void MiniMapView::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_) panMainViewTo(event->pos());
}

void MiniMapView::mouseReleaseEvent(QMouseEvent*)
{
    dragging_ = false;
}

void MiniMapView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    syncViewport();
}

void MiniMapView::panMainViewTo(QPoint miniPos)
{
    if (!mainView_) return;
    QPointF scenePos = mapToScene(miniPos);
    mainView_->centerOn(scenePos);
    update();
}

// ═════════════════════════════════════════════════════════════════════════════
// CFGView
// ═════════════════════════════════════════════════════════════════════════════

CFGView::CFGView(QGraphicsScene* scene, QWidget* parent)
    : QGraphicsView(scene, parent)
{
    setDragMode(NoDrag);
    setTransformationAnchor(AnchorUnderMouse);
    setResizeAnchor(AnchorViewCenter);
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    setBackgroundBrush(clr::base);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
}

void CFGView::fitGraph()
{
    QRectF sr = scene()->sceneRect();
    if (sr.isEmpty()) return;
    fitInView(sr.adjusted(-20, -20, 20, 20), Qt::KeepAspectRatio);
    emit viewportMoved();
}

void CFGView::resetZoom()
{
    resetTransform();
    emit viewportMoved();
}

void CFGView::wheelEvent(QWheelEvent* event)
{
    // Only zoom on Ctrl+scroll; regular scroll pans
    if (event->modifiers() & Qt::ControlModifier) {
        qreal factor = event->angleDelta().y() > 0 ? 1.18 : 1.0 / 1.18;
        scale(factor, factor);
        emit viewportMoved();
        event->accept();
    } else {
        QGraphicsView::wheelEvent(event);
    }
}

void CFGView::mousePressEvent(QMouseEvent* event)
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

void CFGView::mouseMoveEvent(QMouseEvent* event)
{
    if (panning_) {
        QPoint delta = event->pos() - lastPan_;
        lastPan_     = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value()   - delta.y());
        emit viewportMoved();
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void CFGView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        panning_ = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void CFGView::keyPressEvent(QKeyEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_F) { fitGraph();   return; }
        if (event->key() == Qt::Key_0) { resetZoom();  return; }
    }
    QGraphicsView::keyPressEvent(event);
}

void CFGView::scrollContentsBy(int dx, int dy)
{
    QGraphicsView::scrollContentsBy(dx, dy);
    emit viewportMoved();
    if (miniMap_) miniMap_->syncViewport();
}

void CFGView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    repositionMiniMap();
}

void CFGView::repositionMiniMap()
{
    if (miniMap_) {
        miniMap_->move(viewport()->width()  - miniMap_->width()  - 8,
                       8);
        miniMap_->syncViewport();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// CFGScene — layout engine
// ═════════════════════════════════════════════════════════════════════════════

CFGScene::CFGScene(QObject* parent)
    : QGraphicsScene(parent)
{
    setBackgroundBrush(clr::base);
}

void CFGScene::clearGraph()
{
    clear();
    blockItems_.clear();
    compressed_ = false;
}

// ── Encode edge key (from, to) as a single uint64 for the back-edge set ──────
static uint64_t edgeKey(uint64_t from, uint64_t to)
{
    // Simple cantor-like pairing; works as long as ids fit in 32 bits
    return (from << 32) ^ to;
}

// ── Identify back edges via iterative DFS ─────────────────────────────────────
void CFGScene::identifyBackEdges(
    uint64_t entry,
    const std::unordered_map<uint64_t, std::vector<uint64_t>>& succ,
    std::unordered_set<uint64_t>& backSet) const
{
    // DFS colouring: 0=white, 1=grey(in-stack), 2=black
    std::unordered_map<uint64_t, int> color;
    std::stack<std::pair<uint64_t, int>> stk; // (node, succ_index)

    auto it0 = succ.find(entry);
    stk.push({entry, 0});
    color[entry] = 1;

    while (!stk.empty()) {
        auto& [u, idx] = stk.top();
        auto  sit      = succ.find(u);
        bool  pushed   = false;
        if (sit != succ.end()) {
            while (idx < static_cast<int>(sit->second.size())) {
                uint64_t v = sit->second[static_cast<size_t>(idx)];
                ++idx;
                int cv = color.count(v) ? color[v] : 0;
                if (cv == 1) {
                    // back edge
                    backSet.insert(edgeKey(u, v));
                } else if (cv == 0) {
                    color[v] = 1;
                    stk.push({v, 0});
                    pushed = true;
                    break;
                }
            }
        }
        if (!pushed) {
            color[u] = 2;
            stk.pop();
        }
    }
}

// ── BFS rank assignment (longest-path from entry, skipping back edges) ────────
void CFGScene::assignRanks(
    uint64_t entry,
    const std::unordered_map<uint64_t, std::vector<uint64_t>>& succ,
    const std::unordered_set<uint64_t>& backSet,
    std::unordered_map<uint64_t, int>&  rank) const
{
    // Longest-path layering (critical path from entry)
    std::deque<uint64_t> q;
    q.push_back(entry);
    rank[entry] = 0;

    // BFS with rank = max(rank[pred]+1)
    // We do two passes since graph is not a tree
    bool changed = true;
    while (changed) {
        changed = false;
        // Simple BFS-style propagation
        for (auto& [u, succs] : succ) {
            int ru = rank.count(u) ? rank[u] : 0;
            for (uint64_t v : succs) {
                if (backSet.count(edgeKey(u, v))) continue;
                int rv = ru + 1;
                if (!rank.count(v) || rank[v] < rv) {
                    rank[v] = rv;
                    changed  = true;
                }
            }
        }
    }
}

// ── Barycentric crossing minimisation (2 sweeps top-down then bottom-up) ─────
void CFGScene::barycentricSweep(
    std::vector<std::vector<uint64_t>>& levels,
    const std::unordered_map<uint64_t, std::vector<uint64_t>>& pred,
    const std::unordered_map<uint64_t, std::vector<uint64_t>>& succ) const
{
    auto barycentre = [&](uint64_t v, const std::vector<uint64_t>& neighbours,
                          const std::unordered_map<uint64_t, int>& pos) -> double {
        if (neighbours.empty()) return static_cast<double>(pos.count(v) ? pos.at(v) : 0);
        double sum = 0; int cnt = 0;
        for (uint64_t n : neighbours) {
            if (pos.count(n)) { sum += pos.at(n); ++cnt; }
        }
        return cnt > 0 ? sum / cnt : 0;
    };

    // Top-down pass
    for (size_t li = 1; li < levels.size(); ++li) {
        auto& lvl = levels[li];
        // Build position map for level li-1
        std::unordered_map<uint64_t, int> prevPos;
        for (int i = 0; i < static_cast<int>(levels[li-1].size()); ++i)
            prevPos[levels[li-1][static_cast<size_t>(i)]] = i;

        std::stable_sort(lvl.begin(), lvl.end(), [&](uint64_t a, uint64_t b) {
            auto pa = pred.count(a) ? pred.at(a) : std::vector<uint64_t>{};
            auto pb = pred.count(b) ? pred.at(b) : std::vector<uint64_t>{};
            return barycentre(a, pa, prevPos) < barycentre(b, pb, prevPos);
        });
    }

    // Bottom-up pass
    for (int li = static_cast<int>(levels.size()) - 2; li >= 0; --li) {
        auto& lvl = levels[static_cast<size_t>(li)];
        std::unordered_map<uint64_t, int> nextPos;
        auto& nextLvl = levels[static_cast<size_t>(li+1)];
        for (int i = 0; i < static_cast<int>(nextLvl.size()); ++i)
            nextPos[nextLvl[static_cast<size_t>(i)]] = i;

        std::stable_sort(lvl.begin(), lvl.end(), [&](uint64_t a, uint64_t b) {
            auto sa = succ.count(a) ? succ.at(a) : std::vector<uint64_t>{};
            auto sb = succ.count(b) ? succ.at(b) : std::vector<uint64_t>{};
            return barycentre(a, sa, nextPos) < barycentre(b, sb, nextPos);
        });
    }
}

// ── Chain compression (in-degree=1, out-degree=1, not entry/exit/loop-header) ─
std::vector<BasicBlockData> CFGScene::compressChains(
    const std::vector<BasicBlockData>&    in,
    const std::vector<CFGEdgeData>&       inEdges,
    std::vector<CFGEdgeData>&             outEdges) const
{
    // Build adjacency
    std::unordered_map<uint64_t, std::vector<uint64_t>> succ, pred;
    std::unordered_map<uint64_t, const BasicBlockData*> byId;
    for (auto& b : in) byId[b.id] = &b;

    for (auto& e : inEdges) {
        succ[e.from].push_back(e.to);
        pred[e.to].push_back(e.from);
    }

    // Mark nodes that can be compressed (internal chain nodes)
    std::unordered_set<uint64_t> suppressible;
    for (auto& b : in) {
        if (b.isEntry || b.isExit || b.isLoopHeader) continue;
        if (succ[b.id].size() == 1 && pred[b.id].size() == 1) {
            suppressible.insert(b.id);
        }
    }

    // Find chain heads (not suppressible, but their successor is suppressible)
    std::unordered_set<uint64_t> visited;
    std::vector<BasicBlockData> out;

    auto chainEnd = [&](uint64_t head) -> uint64_t {
        uint64_t cur = head;
        while (succ[cur].size() == 1 && suppressible.count(succ[cur][0])) {
            cur = succ[cur][0];
        }
        return cur;
    };

    for (auto& b : in) {
        if (visited.count(b.id)) continue;
        visited.insert(b.id);

        if (!suppressible.count(b.id)) {
            // This is a chain head (or isolated node)
            uint64_t tail = chainEnd(b.id);
            if (tail != b.id) {
                // Walk the chain to count instructions
                int instrCount = static_cast<int>(b.instrs.size());
                uint64_t cur   = b.id;
                while (cur != tail) {
                    cur = succ[cur][0];
                    visited.insert(cur);
                    instrCount += static_cast<int>(byId[cur]->instrs.size());
                }
                // Create a compressed pseudo-block
                BasicBlockData compressed = b;
                // Replace instructions with a single summary
                BlockInstr summary;
                summary.address = b.address;
                summary.text    = QString("[chain: %1 instructions]").arg(instrCount);
                compressed.instrs = {summary};
                out.push_back(compressed);

                // Remap edges: tail → original tail's successor
                // Collect outgoing edges from tail
                for (auto& e : inEdges) {
                    if (e.from == tail) {
                        CFGEdgeData ne = e;
                        ne.from = b.id;
                        outEdges.push_back(ne);
                    }
                }
            } else {
                out.push_back(b);
                for (auto& e : inEdges) {
                    if (e.from == b.id) outEdges.push_back(e);
                }
            }
        }
    }

    return out;
}

// ── Build loop region overlays from loop headers and block loopIds ────────────
void CFGScene::buildLoopRegions(
    const std::vector<BasicBlockData>&    blocks,
    const std::unordered_set<uint64_t>&  /*backSet*/,
    const std::unordered_map<uint64_t, std::vector<uint64_t>>& /*succ*/)
{
    // Group blocks by loopId
    std::unordered_map<int, std::vector<uint64_t>> loopBlocks;
    for (auto& b : blocks) {
        if (b.loopId >= 0) loopBlocks[b.loopId].push_back(b.id);
    }
    if (loopBlocks.empty()) return;

    // Colour palette for loops
    static const QColor kLoopColors[] = {
        clr::blue, clr::mauve, clr::teal, clr::yellow, clr::peach, clr::green,
    };
    int ci = 0;

    for (auto& [lid, bids] : loopBlocks) {
        // Compute bounding rect over all block items in this loop
        QRectF bounds;
        bool   first = true;
        for (uint64_t bid : bids) {
            auto it = blockItems_.find(bid);
            if (it == blockItems_.end()) continue;
            QRectF r = it->second->mapToScene(it->second->boundingRect()).boundingRect();
            if (first) { bounds = r; first = false; }
            else        bounds  = bounds.united(r);
        }
        if (first) continue; // no items found

        bounds.adjust(-12, -12, 12, 12);
        QColor color = kLoopColors[ci % (int)(sizeof(kLoopColors)/sizeof(kLoopColors[0]))];
        ++ci;
        addItem(new LoopRegionItem(bounds, color));
    }
}

// ── Main entry point ──────────────────────────────────────────────────────────
void CFGScene::loadCFG(const std::vector<BasicBlockData>& blocks,
                        const std::vector<CFGEdgeData>&    edges)
{
    clearGraph();
    if (blocks.empty()) return;

    // ── 1. Optionally compress chains ────────────────────────────────────
    std::vector<BasicBlockData> workBlocks = blocks;
    std::vector<CFGEdgeData>    workEdges  = edges;

    if (static_cast<int>(blocks.size()) > kCompressThreshold) {
        compressed_ = true;
        std::vector<CFGEdgeData> compEdges;
        workBlocks = compressChains(blocks, edges, compEdges);
        workEdges  = std::move(compEdges);
    }

    // ── 2. Build adjacency ───────────────────────────────────────────────
    std::unordered_map<uint64_t, std::vector<uint64_t>> succ, pred;
    for (auto& e : workEdges) {
        succ[e.from].push_back(e.to);
        pred[e.to].push_back(e.from);
    }

    // Find entry block
    uint64_t entry = workBlocks.empty() ? 0 : workBlocks[0].id;
    for (auto& b : workBlocks) {
        if (b.isEntry) { entry = b.id; break; }
    }
    // Fallback: node with no predecessors
    if (!workBlocks.empty()) {
        for (auto& b : workBlocks) {
            if (pred[b.id].empty()) { entry = b.id; break; }
        }
    }

    // ── 3. Back-edge identification ───────────────────────────────────────
    std::unordered_set<uint64_t> backSet;
    identifyBackEdges(entry, succ, backSet);

    // ── 4. Rank assignment ────────────────────────────────────────────────
    std::unordered_map<uint64_t, int> rank;
    assignRanks(entry, succ, backSet, rank);

    // Ensure all nodes have a rank
    for (auto& b : workBlocks) {
        if (!rank.count(b.id)) rank[b.id] = 0;
    }

    // ── 5. Build level lists ─────────────────────────────────────────────
    int maxRank = 0;
    for (auto& [id, r] : rank) maxRank = std::max(maxRank, r);

    std::vector<std::vector<uint64_t>> levels(static_cast<size_t>(maxRank + 1));
    for (auto& b : workBlocks) {
        levels[static_cast<size_t>(rank[b.id])].push_back(b.id);
    }

    // ── 6. Crossing minimisation ──────────────────────────────────────────
    barycentricSweep(levels, pred, succ);

    // ── 7. Coordinate assignment ─────────────────────────────────────────
    std::unordered_map<uint64_t, QPointF> nodePos;
    std::unordered_map<uint64_t, const BasicBlockData*> byId;
    for (auto& b : workBlocks) byId[b.id] = &b;

    for (int ri = 0; ri <= maxRank; ++ri) {
        auto& lvl = levels[static_cast<size_t>(ri)];
        int   N   = static_cast<int>(lvl.size());
        for (int ci2 = 0; ci2 < N; ++ci2) {
            uint64_t id = lvl[static_cast<size_t>(ci2)];
            qreal    x  = kMargin + ci2 * kNodeSpacingH;
            qreal    y  = kMargin + ri  * kLevelH;
            nodePos[id] = QPointF(x, y);
        }
    }

    // ── 8. Create BasicBlockItems ─────────────────────────────────────────
    for (auto& b : workBlocks) {
        auto* item = new BasicBlockItem(b);
        QPointF p  = nodePos.count(b.id) ? nodePos[b.id] : QPointF(0, 0);
        item->setPos(p);
        addItem(item);
        blockItems_[b.id] = item;
        connect(item, &BasicBlockItem::clicked,
                this,  &CFGScene::blockClicked);
    }

    // ── 9. Create edges ───────────────────────────────────────────────────
    for (auto& e : workEdges) {
        auto fIt = blockItems_.find(e.from);
        auto tIt = blockItems_.find(e.to);
        if (fIt == blockItems_.end() || tIt == blockItems_.end()) continue;

        QPointF from = fIt->second->bottomCentre();
        QPointF to   = tIt->second->topCentre();

        bool isBack = backSet.count(edgeKey(e.from, e.to)) > 0
                   || e.kind == EdgeKind::BackEdge;

        auto* ei = new CFGEdgeItem(from, to, isBack ? EdgeKind::BackEdge : e.kind, isBack);
        addItem(ei);
    }

    // ── 10. Loop region overlays ──────────────────────────────────────────
    buildLoopRegions(workBlocks, backSet, succ);

    // Update scene rect with padding
    setSceneRect(itemsBoundingRect().adjusted(-kMargin, -kMargin, kMargin, kMargin));
}

// ═════════════════════════════════════════════════════════════════════════════
// CFGPanel
// ═════════════════════════════════════════════════════════════════════════════

CFGPanel::CFGPanel(QWidget* parent)
    : PanelBase("Control Flow Graph", parent)
{
    setupUI();
}

void CFGPanel::setupUI()
{
    // ── Top toolbar ──────────────────────────────────────────────────────
    auto* topBar    = new QWidget(this);
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(6, 3, 6, 3);
    topLayout->setSpacing(6);

    funcLabel_ = new QLabel("No function selected", topBar);
    funcLabel_->setProperty("role", "muted");

    fitButton_   = new QPushButton("Fit",          topBar);
    resetButton_ = new QPushButton("100%",         topBar);
    svgButton_   = new QPushButton("Export SVG",   topBar);
    pngButton_   = new QPushButton("Export PNG",   topBar);
    infoLabel_   = new QLabel("",                  topBar);
    infoLabel_->setProperty("role", "muted");

    for (auto* btn : {fitButton_, resetButton_, svgButton_, pngButton_}) {
        btn->setFixedHeight(24);
        btn->setStyleSheet(
            "QPushButton { background: #313244; color: #cdd6f4; border: 1px solid #45475a;"
            " border-radius: 4px; padding: 0 8px; }"
            "QPushButton:hover { background: #45475a; }"
            "QPushButton:pressed { background: #585b70; }");
    }

    topLayout->addWidget(funcLabel_, 1);
    topLayout->addWidget(infoLabel_);
    topLayout->addWidget(fitButton_);
    topLayout->addWidget(resetButton_);
    topLayout->addWidget(svgButton_);
    topLayout->addWidget(pngButton_);

    // ── Scene & views ─────────────────────────────────────────────────────
    scene_     = new CFGScene(this);
    graphView_ = new CFGView(scene_, this);
    graphHost_ = new QWidget(this);
    auto* graphLay = new QVBoxLayout(graphHost_);
    graphLay->setContentsMargins(0, 0, 0, 0);
    graphLay->addWidget(graphView_);
    // Parent minimap to the viewport widget so it overlays the canvas properly
    miniMap_   = new MiniMapView(scene_, graphView_, graphView_->viewport());
    graphView_->setMiniMap(miniMap_);
    miniMap_->raise();

    emptyState_ = new retdec::gui::widgets::EmptyStateWidget(this);
    emptyState_->setTitle(QStringLiteral("No control-flow graph"));
    emptyState_->setHint(QStringLiteral("Select a function to visualize its basic blocks and edges."));

    bodyStack_ = new QStackedWidget(this);
    bodyStack_->addWidget(emptyState_);
    bodyStack_->addWidget(graphHost_);

    // ── Layout ────────────────────────────────────────────────────────────
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(topBar);
    layout->addWidget(bodyStack_, 1);

    // ── Connections ───────────────────────────────────────────────────────
    connect(fitButton_,   &QPushButton::clicked, this, &CFGPanel::onFitView);
    connect(resetButton_, &QPushButton::clicked, this, &CFGPanel::onResetZoom);
    connect(svgButton_,   &QPushButton::clicked, this, &CFGPanel::onExportSvg);
    connect(pngButton_,   &QPushButton::clicked, this, &CFGPanel::onExportPng);
    connect(scene_, &CFGScene::blockClicked, this, &CFGPanel::onBlockClicked);
    connect(graphView_, &CFGView::viewportMoved, miniMap_, &MiniMapView::syncViewport);
    updateEmptyState();
}

void CFGPanel::updateEmptyState() {
    if (!bodyStack_ || !scene_) return;
    bodyStack_->setCurrentIndex(scene_->items().isEmpty() ? 0 : 1);
}

void CFGPanel::loadCFG(const std::vector<BasicBlockData>& blocks,
                        const std::vector<CFGEdgeData>&    edges)
{
    addressMap_.clear();
    for (auto& b : blocks) addressMap_[b.id] = b.address;

    scene_->loadCFG(blocks, edges);

    // Update info label
    QString info = QString("%1 blocks").arg(blocks.size());
    if (scene_->isCompressed()) info += " (compressed)";
    infoLabel_->setText(info);

    graphView_->fitGraph();
    miniMap_->syncViewport();
    updateEmptyState();
}

void CFGPanel::clear()
{
    scene_->clearGraph();
    funcLabel_->setText("No function selected");
    infoLabel_->setText("");
    addressMap_.clear();
    updateEmptyState();
}

void CFGPanel::onFunctionSelected(uint64_t address, const QString& name)
{
    funcLabel_->setText(QString("%1  0x%2")
                        .arg(name)
                        .arg(address, 0, 16));
}

void CFGPanel::onFitView()
{
    graphView_->fitGraph();
}

void CFGPanel::onResetZoom()
{
    graphView_->resetZoom();
}

void CFGPanel::onExportSvg()
{
#ifdef QT_SVG_LIB
    QString path = QFileDialog::getSaveFileName(
        this, "Export CFG as SVG", QString(), "SVG files (*.svg)");
    if (path.isEmpty()) return;

    QRectF   sr = scene_->sceneRect();
    QSvgGenerator gen;
    gen.setFileName(path);
    gen.setSize(sr.size().toSize());
    gen.setViewBox(sr);
    gen.setTitle("RetDec CFG Export");

    QPainter painter;
    painter.begin(&gen);
    painter.setRenderHint(QPainter::Antialiasing);
    scene_->render(&painter, QRectF(), sr);
    painter.end();
#else
    QMessageBox::warning(this, "SVG Export",
        "SVG export requires Qt6::Svg. Rebuild with -DQT_SVG=ON.");
#endif
}

void CFGPanel::onExportPng()
{
    QString path = QFileDialog::getSaveFileName(
        this, "Export CFG as PNG", QString(), "PNG files (*.png)");
    if (path.isEmpty()) return;

    QRectF sr = scene_->sceneRect().adjusted(-10, -10, 10, 10);
    if (sr.isEmpty()) return;

    QImage img(sr.size().toSize() * 2, QImage::Format_ARGB32_Premultiplied);
    img.fill(clr::base);
    img.setDevicePixelRatio(2.0);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing);
    scene_->render(&painter, QRectF(), sr);
    painter.end();

    if (!img.save(path)) {
        QMessageBox::warning(this, "Export PNG", "Failed to save PNG: " + path);
    }
}

void CFGPanel::onBlockClicked(uint64_t blockId)
{
    auto it = addressMap_.find(blockId);
    if (it != addressMap_.end()) {
        emit blockNavigationRequested(it->second);
    }
}

} // namespace retdec::gui::panels
