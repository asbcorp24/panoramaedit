#include "ToolIcons.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPolygonF>
#include <QtMath>

namespace {
void arrow(QPainter &p, QPointF from, QPointF to) {
    p.drawLine(from, to);
    const QPointF direction = to - from;
    const qreal length = qSqrt(direction.x() * direction.x() + direction.y() * direction.y());
    if (length < 0.01) return;
    const QPointF unit = direction / length;
    const QPointF perpendicular(-unit.y(), unit.x());
    p.drawLine(to, to - unit * 4.0 + perpendicular * 2.6);
    p.drawLine(to, to - unit * 4.0 - perpendicular * 2.6);
}

void sparkle(QPainter &p, QPointF center, qreal radius) {
    QPainterPath rays;
    rays.moveTo(center.x(), center.y() - radius);
    rays.quadTo(center.x() + radius * .24, center.y() - radius * .24,
                center.x() + radius, center.y());
    rays.quadTo(center.x() + radius * .24, center.y() + radius * .24,
                center.x(), center.y() + radius);
    rays.quadTo(center.x() - radius * .24, center.y() + radius * .24,
                center.x() - radius, center.y());
    rays.quadTo(center.x() - radius * .24, center.y() - radius * .24,
                center.x(), center.y() - radius);
    p.drawPath(rays);
}
}

namespace ToolIcons {
QIcon make(Kind kind) {
    QPixmap pixels(64, 64);
    pixels.fill(Qt::transparent);
    QPainter p(&pixels);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(2, 2);
    const QColor ink = QGuiApplication::palette().color(QPalette::ButtonText);
    const QColor accent(24, 132, 210);
    const QColor secondary(65, 160, 220);
    const QPen mainPen(ink, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    const QPen accentPen(accent, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(mainPen);
    p.setBrush(Qt::NoBrush);

    switch (kind) {
    case Kind::Navigate: {
        p.drawEllipse(QPointF(16, 16), 9, 9);
        p.setPen(accentPen);
        p.drawArc(QRectF(3, 9, 26, 14), 20 * 16, 300 * 16);
        p.setBrush(accent);
        p.drawEllipse(QPointF(26.7, 13.7), 2.2, 2.2);
        break;
    }
    case Kind::Brush: {
        QPainterPath bristles;
        bristles.moveTo(11, 20);
        bristles.cubicTo(5, 17, 8, 27, 4.5, 28);
        bristles.cubicTo(12, 29, 17, 27, 14, 22);
        bristles.closeSubpath();
        p.setBrush(accent);
        p.setPen(accentPen);
        p.drawPath(bristles);
        p.setBrush(Qt::NoBrush);
        p.setPen(mainPen);
        p.drawLine(QPointF(13, 19), QPointF(25, 5));
        p.drawLine(QPointF(17, 21), QPointF(28, 8));
        p.drawLine(QPointF(25, 5), QPointF(28, 8));
        break;
    }
    case Kind::Eraser: {
        QPolygonF shape{QPointF(6, 19), QPointF(18, 5), QPointF(28, 14),
                        QPointF(16, 27), QPointF(11, 27)};
        p.setBrush(accent.lighter(175));
        p.drawPolygon(shape);
        p.setPen(accentPen);
        p.drawLine(QPointF(11, 13), QPointF(21, 22));
        p.setPen(mainPen);
        p.drawLine(QPointF(6, 28), QPointF(29, 28));
        break;
    }
    case Kind::Stamp: {
        p.drawRoundedRect(QRectF(12, 5, 8, 8), 2, 2);
        p.drawLine(QPointF(14, 13), QPointF(11, 19));
        p.drawLine(QPointF(18, 13), QPointF(21, 19));
        p.setBrush(accent.lighter(170));
        p.drawRoundedRect(QRectF(6, 19, 20, 7), 2, 2);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(5, 28), QPointF(27, 28));
        break;
    }
    case Kind::Smudge: {
        QPainterPath finger;
        finger.moveTo(11, 27);
        finger.cubicTo(9, 23, 5, 18, 7, 16);
        finger.cubicTo(9, 14, 12, 18, 13, 20);
        finger.lineTo(13, 7);
        finger.cubicTo(13, 3, 17, 3, 17, 7);
        finger.lineTo(17, 16);
        finger.cubicTo(18, 12, 22, 13, 22, 16);
        finger.cubicTo(25, 13, 28, 16, 26, 20);
        finger.lineTo(23, 27);
        finger.closeSubpath();
        p.setBrush(accent.lighter(180));
        p.drawPath(finger);
        p.setBrush(Qt::NoBrush);
        p.setPen(accentPen);
        p.drawLine(QPointF(5, 10), QPointF(9, 7));
        break;
    }
    case Kind::Push: {
        QPainterPath curve;
        curve.moveTo(5, 24);
        curve.cubicTo(7, 13, 16, 25, 20, 13);
        curve.cubicTo(21, 10, 25, 8, 28, 8);
        p.drawPath(curve);
        p.setPen(accentPen);
        arrow(p, QPointF(19, 22), QPointF(27, 14));
        break;
    }
    case Kind::Bloat: {
        p.drawEllipse(QPointF(16, 16), 6, 6);
        p.setPen(accentPen);
        arrow(p, QPointF(16, 10), QPointF(16, 3));
        arrow(p, QPointF(22, 16), QPointF(29, 16));
        arrow(p, QPointF(16, 22), QPointF(16, 29));
        arrow(p, QPointF(10, 16), QPointF(3, 16));
        break;
    }
    case Kind::Pinch: {
        p.drawEllipse(QPointF(16, 16), 5, 5);
        p.setPen(accentPen);
        arrow(p, QPointF(16, 3), QPointF(16, 10));
        arrow(p, QPointF(29, 16), QPointF(22, 16));
        arrow(p, QPointF(16, 29), QPointF(16, 22));
        arrow(p, QPointF(3, 16), QPointF(10, 16));
        break;
    }
    case Kind::Sharpen: {
        p.setPen(accentPen);
        sparkle(p, QPointF(16, 15), 10);
        sparkle(p, QPointF(26, 7), 3);
        p.setPen(mainPen);
        p.drawLine(QPointF(4, 27), QPointF(13, 27));
        break;
    }
    case Kind::Inpaint: {
        p.drawRoundedRect(QRectF(4, 4, 24, 24), 2, 2);
        p.setPen(QPen(accent, 2, Qt::DashLine));
        p.drawRect(QRectF(8, 8, 11, 11));
        p.setPen(mainPen);
        p.drawLine(QPointF(17, 23), QPointF(26, 14));
        p.setPen(accentPen);
        sparkle(p, QPointF(24, 8), 4);
        break;
    }
    case Kind::Color: {
        p.setPen(mainPen);
        p.drawEllipse(QRectF(4, 5, 24, 22));
        const QColor colors[] = {QColor(230, 74, 74), QColor(54, 162, 85),
                                 QColor(43, 121, 220), QColor(235, 177, 44)};
        const QPointF positions[] = {{11, 11}, {21, 11}, {11, 20}, {21, 20}};
        for (int i = 0; i < 4; ++i) {
            p.setBrush(colors[i]);
            p.setPen(Qt::NoPen);
            p.drawEllipse(positions[i], 3.2, 3.2);
        }
        break;
    }
    case Kind::ApplyAi: {
        p.drawLine(QPointF(7, 26), QPointF(22, 11));
        p.setPen(accentPen);
        sparkle(p, QPointF(24, 8), 6);
        sparkle(p, QPointF(9, 9), 3);
        break;
    }
    case Kind::ClearAi: {
        p.drawRoundedRect(QRectF(5, 7, 22, 18), 3, 3);
        p.setPen(accentPen);
        p.drawLine(QPointF(12, 12), QPointF(21, 21));
        p.drawLine(QPointF(21, 12), QPointF(12, 21));
        break;
    }
    case Kind::Seam: {
        p.drawLine(QPointF(8, 5), QPointF(8, 27));
        p.drawLine(QPointF(24, 5), QPointF(24, 27));
        p.setPen(accentPen);
        arrow(p, QPointF(2, 16), QPointF(13, 16));
        arrow(p, QPointF(30, 16), QPointF(19, 16));
        break;
    }
    }
    p.end();
    pixels.setDevicePixelRatio(2.0);
    return QIcon(pixels);
}
} // namespace ToolIcons
