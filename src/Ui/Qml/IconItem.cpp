#include "Ui/Qml/IconItem.hpp"

#include <QPainter>

#include <algorithm>

#include "Ui/Icons.hpp"
#include "Ui/Theme.hpp"

namespace ui {

IconItem::IconItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    setPerformanceHint(QQuickPaintedItem::FastFBOResizing, true);
}

void IconItem::setKind(int kind) {
    if (m_kind == kind) return;
    m_kind = kind;
    Q_EMIT kindChanged();
    update();
}

void IconItem::setColor(const QColor& color) {
    if (m_color == color) return;
    m_color = color;
    Q_EMIT colorChanged();
    update();
}

void IconItem::setAngle(qreal angle) {
    if (qFuzzyCompare(m_angle, angle)) return;
    m_angle = angle;
    Q_EMIT angleChanged();
    update();
}

void IconItem::paint(QPainter* painter) {
    const QColor ink = m_color.isValid() ? m_color : Theme::instance().current().fg2;
    const qreal box = std::min(width(), height());
    if (box <= 0.0) return;
    painter->translate(width() / 2.0, height() / 2.0);
    PaintIcon(*painter, static_cast<IconKind>(m_kind), ink, box, m_angle);
}

}  // namespace ui
