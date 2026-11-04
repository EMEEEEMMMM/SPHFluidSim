#include "Ui/Icons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

namespace ui {

void PaintIcon(QPainter& painter, IconKind kind, const QColor& ink, qreal sizePx,
               qreal rotationDegrees) {
    QPen stroke(ink, 1.6);
    stroke.setCosmetic(true);
    stroke.setCapStyle(Qt::RoundCap);
    stroke.setJoinStyle(Qt::RoundJoin);

    const qreal scale = sizePx / 16.0;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(-8.0 * scale, -8.0 * scale);
    painter.rotate(rotationDegrees);
    painter.scale(scale, scale);
    painter.setPen(stroke);
    painter.setBrush(Qt::NoBrush);

    auto filled = [&](const QPainterPath& path) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawPath(path);
    };

    switch (kind) {
        case IconKind::Play: {
            QPainterPath path;
            path.moveTo(5.6, 3.4);
            path.lineTo(12.6, 8.0);
            path.lineTo(5.6, 12.6);
            path.closeSubpath();
            filled(path);
            break;
        }
        case IconKind::Pause: {
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            painter.drawRoundedRect(QRectF(5.0, 3.5, 2.5, 9.0), 0.9, 0.9);
            painter.drawRoundedRect(QRectF(8.5, 3.5, 2.5, 9.0), 0.9, 0.9);
            break;
        }
        case IconKind::Step: {
            QPainterPath path;
            path.moveTo(4.4, 3.6);
            path.lineTo(10.0, 8.0);
            path.lineTo(4.4, 12.4);
            path.closeSubpath();
            filled(path);
            painter.setPen(stroke);
            painter.drawLine(QPointF(12.2, 3.6), QPointF(12.2, 12.4));
            break;
        }
        case IconKind::Reset: {
            painter.drawArc(QRectF(3.4, 3.6, 9.2, 9.2), 55 * 16, 290 * 16);
            QPainterPath head;
            head.moveTo(9.0, 1.5);
            head.lineTo(11.6, 4.4);
            head.lineTo(8.2, 5.6);
            head.closeSubpath();
            filled(head);
            break;
        }
        case IconKind::Camera: {
            painter.drawRoundedRect(QRectF(2.2, 5.2, 11.6, 8.0), 2.2, 2.2);
            QPainterPath hump;
            hump.moveTo(5.4, 5.2);
            hump.lineTo(6.6, 3.4);
            hump.lineTo(9.6, 3.4);
            hump.lineTo(10.8, 5.2);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(stroke);
            painter.drawPath(hump);
            painter.drawEllipse(QPointF(8.0, 9.2), 2.4, 2.4);
            break;
        }
        case IconKind::Fullscreen: {
            painter.drawPolyline(QPolygonF() << QPointF(6.0, 3.4) << QPointF(3.4, 3.4)
                                             << QPointF(3.4, 6.0));
            painter.drawPolyline(QPolygonF() << QPointF(10.0, 3.4) << QPointF(12.6, 3.4)
                                             << QPointF(12.6, 6.0));
            painter.drawPolyline(QPolygonF() << QPointF(6.0, 12.6) << QPointF(3.4, 12.6)
                                             << QPointF(3.4, 10.0));
            painter.drawPolyline(QPolygonF() << QPointF(10.0, 12.6) << QPointF(12.6, 12.6)
                                             << QPointF(12.6, 10.0));
            break;
        }
        case IconKind::Palette: {
            painter.drawEllipse(QPointF(8.0, 8.0), 5.6, 5.6);
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            painter.drawEllipse(QPointF(6.0, 6.2), 1.0, 1.0);
            painter.drawEllipse(QPointF(10.0, 6.2), 1.0, 1.0);
            painter.drawEllipse(QPointF(5.6, 9.6), 1.0, 1.0);
            break;
        }
        case IconKind::Save: {
            painter.drawLine(QPointF(8.0, 2.8), QPointF(8.0, 10.2));
            painter.drawPolyline(QPolygonF() << QPointF(5.2, 7.6) << QPointF(8.0, 10.6)
                                             << QPointF(10.8, 7.6));
            painter.drawLine(QPointF(3.6, 13.0), QPointF(12.4, 13.0));
            break;
        }
        case IconKind::Chevron: {
            painter.drawPolyline(QPolygonF() << QPointF(6.2, 4.0) << QPointF(10.0, 8.0)
                                             << QPointF(6.2, 12.0));
            break;
        }
        case IconKind::Droplet: {
            QPainterPath drop;
            drop.moveTo(8.0, 2.3);
            drop.cubicTo(10.8, 6.0, 12.5, 8.1, 12.5, 10.1);
            drop.cubicTo(12.5, 12.5, 10.5, 13.8, 8.0, 13.8);
            drop.cubicTo(5.5, 13.8, 3.5, 12.5, 3.5, 10.1);
            drop.cubicTo(3.5, 8.1, 5.2, 6.0, 8.0, 2.3);
            filled(drop);
            break;
        }
        case IconKind::WindowMinimize: {
            painter.drawLine(QPointF(4.0, 8.0), QPointF(12.0, 8.0));
            break;
        }
        case IconKind::WindowMaximize: {
            painter.drawRect(QRectF(4.2, 4.2, 7.6, 7.6));
            break;
        }
        case IconKind::WindowClose: {
            painter.drawLine(QPointF(4.3, 4.3), QPointF(11.7, 11.7));
            painter.drawLine(QPointF(11.7, 4.3), QPointF(4.3, 11.7));
            break;
        }
    }
    painter.restore();
}

}  // namespace ui
