#include "SphericalLiquify.h"
#include "SphericalRaster.h"

#include <QQuaternion>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;

double clamp1(double value) { return std::clamp(value, -1.0, 1.0); }

QVector3D normalizedOr(const QVector3D &value, const QVector3D &fallback) {
    return value.lengthSquared() > 1e-12f ? value.normalized() : fallback;
}

struct PixelChange {
    int x;
    int y;
    QRgb color;
};

QRgb blend(QRgb current, QRgb dragged, double amount) {
    const auto channel = [amount](int a, int b) {
        return std::clamp(int(std::lround(a + (b - a) * amount)), 0, 255);
    };
    return qRgba(channel(qRed(current), qRed(dragged)),
                 channel(qGreen(current), qGreen(dragged)),
                 channel(qBlue(current), qBlue(dragged)),
                 channel(qAlpha(current), qAlpha(dragged)));
}
} // namespace

namespace SphericalLiquify {
QRegion apply(QImage &layer, QImage &composite, const QImage &source,
              const QVector3D &center, const QVector3D &previous,
              double radiusDegrees, double strength, Tool tool) {
    if (layer.isNull() || composite.isNull() || source.isNull() ||
        layer.size() != composite.size() || layer.size() != source.size() ||
        center.lengthSquared() < 1e-12f || previous.lengthSquared() < 1e-12f)
        return {};

    const QVector3D anchor = center.normalized();
    const QVector3D from = previous.normalized();
    const bool dragging = tool == Tool::Smudge || tool == Tool::Push;
    if (dragging && (anchor - from).lengthSquared() < 1e-12f)
        return {}; // No movement: no smear or warp.

    const int width = layer.width(), height = layer.height();
    const double radius = std::clamp(radiusDegrees, 0.1, 30.0) * pi / 180.0;
    const double intensity = std::clamp(strength, 0.01, 1.0);
    const double latitude = std::asin(clamp1(anchor.y()));
    const int cy = std::clamp(int((0.5 - latitude / pi) * height), 0, height - 1);
    const int ry = int(std::ceil(radius * height / pi)) + 2;
    const double longitude = std::atan2(anchor.x(), -anchor.z());
    const QQuaternion rotation = dragging ? QQuaternion::rotationTo(anchor, from)
        : QQuaternion(1.0f, 0.0f, 0.0f, 0.0f);
    const QQuaternion identity(1.0f, 0.0f, 0.0f, 0.0f);

    // The source is allowed to alias composite. Collect the entire dab before
    // writing it, so the next dab sees the previous result but each dab reads
    // a consistent image. Unlike QImage::copy(), this costs only brush area,
    // not a full 8K/16K panorama on every mouse movement.
    std::vector<PixelChange> changes;
    QRegion dirty;
    for (int y = std::max(0, cy - ry); y <= std::min(height - 1, cy + ry); ++y) {
        const double phi = pi * (0.5 - (y + 0.5) / height);
        const double denominator = std::cos(phi) * std::cos(latitude);
        const double numerator = std::cos(radius) - std::sin(phi) * std::sin(latitude);
        // Avoid dividing by zero at either pole.
        const double threshold = denominator > 1e-8 ? numerator / denominator
            : (numerator <= 0.0 ? -1.0 : 2.0);
        if (threshold > 1.0) continue;
        const double span = threshold <= -1.0 ? pi : std::acos(clamp1(threshold));
        const int cx = int((longitude / (2.0 * pi) + 0.5) * width);
        const int rx = int(std::ceil(span * width / (2.0 * pi))) + 2;
        const bool fullRow = rx >= width / 2;
        const int first = fullRow ? 0 : cx - rx;
        const int last = fullRow ? width - 1 : cx + rx;
        bool rowChanged = false;
        int firstChanged = 0, lastChanged = -1;
        const uchar *sourceRow = source.constScanLine(y);

        for (int xi = first; xi <= last; ++xi) {
            const int x = (xi % width + width) % width;
            const QVector3D direction = SphericalRaster::pixelDirection(x, y, width, height);
            const double angle = std::acos(clamp1(QVector3D::dotProduct(direction, anchor)));
            if (angle >= radius) continue;
            const double effect = intensity * std::pow(1.0 - angle / radius, 2.0);

            QVector3D sampleDirection = direction;
            if (dragging) {
                // Reverse-map pixels along the geodesic cursor movement.
                const QQuaternion partial = QQuaternion::slerp(identity, rotation, float(effect));
                sampleDirection = partial.rotatedVector(direction).normalized();
            } else {
                const QVector3D tangent = normalizedOr(
                    direction - anchor * float(std::cos(angle)), direction);
                // Both radial tools hold the centre fixed and decay to zero
                // at the brush edge, so repeated dabs accumulate naturally.
                const double sampleAngle = tool == Tool::Bloat
                    ? angle * (1.0 - 0.85 * effect)
                    : angle * (1.0 + 0.85 * effect);
                sampleDirection = normalizedOr(anchor * float(std::cos(sampleAngle)) +
                    tangent * float(std::sin(sampleAngle)), direction);
            }

            const QRgb sampled = SphericalRaster::sampleDirection(source, sampleDirection);
            const uchar *src = sourceRow + x * 4;
            const QRgb current = qRgba(src[0], src[1], src[2], src[3]);
            const QRgb result = tool == Tool::Smudge ? blend(current, sampled, 0.7) : sampled;
            if (result == current) continue;
            changes.push_back({x, y, result});
            if (!rowChanged) firstChanged = xi;
            lastChanged = xi;
            rowChanged = true;
        }
        if (rowChanged) {
            const int start = (firstChanged % width + width) % width;
            const int count = lastChanged - firstChanged + 1;
            // Split seam-crossing regions instead of uploading a whole 16K
            // texture row for a tiny brush touching both image edges.
            if (count >= width) dirty += QRect(0, y, width, 1);
            else if (start + count <= width) dirty += QRect(start, y, count, 1);
            else {
                dirty += QRect(start, y, width - start, 1);
                dirty += QRect(0, y, start + count - width, 1);
            }
        }
    }

    // Writing only after all reads also avoids smearing in the wrong direction
    // due to the order of source pixels in memory.
    int lastY = -1;
    uchar *layerRow = nullptr;
    uchar *compositeRow = nullptr;
    for (const PixelChange &change : changes) {
        if (change.y != lastY) {
            layerRow = layer.scanLine(change.y);
            compositeRow = composite.scanLine(change.y);
            lastY = change.y;
        }
        uchar *edited = layerRow + change.x * 4;
        edited[0] = qRed(change.color);
        edited[1] = qGreen(change.color);
        edited[2] = qBlue(change.color);
        edited[3] = 255;
        uchar *shown = compositeRow + change.x * 4;
        shown[0] = edited[0];
        shown[1] = edited[1];
        shown[2] = edited[2];
        shown[3] = 255;
    }
    return dirty;
}
} // namespace SphericalLiquify
