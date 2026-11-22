#include "SphericalLiquify.h"
#include "SphericalRaster.h"
#include <QQuaternion>
#include <algorithm>
#include <cmath>

namespace {
constexpr double PI = 3.14159265358979323846;
double clamp1(double v) { return std::clamp(v, -1.0, 1.0); }
QVector3D normalizeSafe(const QVector3D &v, const QVector3D &fallback) {
    return v.lengthSquared() > 1e-12f ? v.normalized() : fallback;
}
}
namespace SphericalLiquify {
QRegion apply(QImage &layer, QImage &composite, const QImage &source,
              const QVector3D &center, const QVector3D &previous,
              double radiusDegrees, double strength, Tool tool) {
    if (layer.isNull() || composite.isNull() || source.isNull()) return {};
    const int w = layer.width(), h = layer.height();
    const double radius = std::clamp(radiusDegrees, 0.1, 30.0) * PI / 180.0;
    strength = std::clamp(strength, 0.01, 1.0);
    const double latitude = std::asin(clamp1(center.y()));
    const int cy = std::clamp(int((0.5 - latitude / PI) * h), 0, h - 1);
    const int ry = int(std::ceil(radius * h / PI)) + 2;
    const double longitude = std::atan2(center.x(), -center.z());
    const QQuaternion strokeRotation = QQuaternion::rotationTo(center, previous);
    QRegion dirty;
    for (int y = std::max(0, cy - ry); y <= std::min(h - 1, cy + ry); ++y) {
        const double phi = PI * (0.5 - (y + 0.5) / h);
        const double denom = std::cos(phi) * std::cos(latitude);
        const double threshold = (std::cos(radius) - std::sin(phi) * std::sin(latitude)) / denom;
        if (threshold > 1.0) continue;
        const double span = denom < 1e-8 || threshold <= -1.0 ? PI : std::acos(clamp1(threshold));
        const int cx = int((longitude / (2.0 * PI) + 0.5) * w);
        const int rx = int(std::ceil(span * w / (2.0 * PI))) + 2;
        const bool all = rx >= w / 2;
        const int first = all ? 0 : cx - rx, last = all ? w - 1 : cx + rx;
        uchar *dst = layer.scanLine(y);
        uchar *visible = composite.scanLine(y);
        for (int xi = first; xi <= last; ++xi) {
            const int x = (xi % w + w) % w;
            const QVector3D d = SphericalRaster::pixelDirection(x, y, w, h);
            const double angle = std::acos(clamp1(QVector3D::dotProduct(d, center)));
            if (angle >= radius) continue;
            const double falloff = std::pow(1.0 - angle / radius, 2.0) * strength;
            QVector3D sample = d;
            if (tool == Tool::Smudge || tool == Tool::Push) {
                // Move the source backward along the spherical stroke, with
                // a soft radial falloff; no flat-image offset near the poles.
                const QQuaternion partial = QQuaternion::slerp(QQuaternion(), strokeRotation, float(falloff));
                sample = partial.rotatedVector(d).normalized();
            } else {
                // Tangent vector from centre to the destination point.
                const QVector3D tangent = normalizeSafe(d - center * float(std::cos(angle)), d);
                const double sourceAngle = tool == Tool::Bloat
                    ? angle * (1.0 - 0.75 * falloff)
                    : angle + (radius - angle) * 0.75 * falloff;
                sample = normalizeSafe(center * float(std::cos(sourceAngle)) +
                    tangent * float(std::sin(sourceAngle)), d);
            }
            const QRgb pixel = SphericalRaster::sampleDirection(source, sample);
            uchar *out = dst + x * 4;
            out[0] = qRed(pixel); out[1] = qGreen(pixel);
            out[2] = qBlue(pixel); out[3] = 255;
            uchar *shown = visible + x * 4;
            shown[0] = out[0]; shown[1] = out[1];
            shown[2] = out[2]; shown[3] = 255;
            dirty += QRect(x, y, 1, 1);
        }
    }
    return dirty;
}
}
