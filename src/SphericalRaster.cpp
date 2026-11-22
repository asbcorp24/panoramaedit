#include "SphericalRaster.h"

#include <QPointF>
#include <algorithm>
#include <cmath>

namespace {
constexpr float pi = 3.14159265358979323846f;

float clampUnit(float value) { return std::clamp(value, -1.0f, 1.0f); }
int wrappedX(int x, int width) { return (x % width + width) % width; }

QRgb pixelAt(const QImage &image, int x, int y) {
    const uchar *pixel = image.constScanLine(y) + 4 * x;
    return qRgba(pixel[0], pixel[1], pixel[2], pixel[3]);
}

void writePixel(uchar *pixel, QRgb rgb) {
    pixel[0] = uchar(qRed(rgb));
    pixel[1] = uchar(qGreen(rgb));
    pixel[2] = uchar(qBlue(rgb));
    pixel[3] = uchar(qAlpha(rgb));
}

QRgb over(QRgb foreground, QRgb background) {
    const int fa = qAlpha(foreground);
    const int ba = qAlpha(background);
    const int alpha = fa + (ba * (255 - fa) + 127) / 255;
    if (alpha == 0) return qRgba(0, 0, 0, 0);
    const auto channel = [=](int f, int b) {
        const int premul = f * fa + (b * ba * (255 - fa) + 127) / 255;
        return std::clamp((premul + alpha / 2) / alpha, 0, 255);
    };
    return qRgba(channel(qRed(foreground), qRed(background)),
                 channel(qGreen(foreground), qGreen(background)),
                 channel(qBlue(foreground), qBlue(background)), alpha);
}

QPointF imageCoordinate(const QVector3D &direction, int width, int height) {
    const float longitude = std::atan2(direction.x(), -direction.z());
    const float latitude = std::asin(clampUnit(direction.y()));
    return QPointF((longitude / (2 * pi) + 0.5f) * width - 0.5,
                   (0.5f - latitude / pi) * height - 0.5);
}
} // namespace

namespace SphericalRaster {

QVector3D pixelDirection(int x, int y, int width, int height) {
    const float longitude = (float(x + 0.5f) / width - 0.5f) * 2 * pi;
    const float latitude = (0.5f - float(y + 0.5f) / height) * pi;
    return QVector3D(std::cos(latitude) * std::sin(longitude),
                     std::sin(latitude),
                     -std::cos(latitude) * std::cos(longitude));
}

QRgb sampleDirection(const QImage &image, const QVector3D &direction) {
    if (image.isNull()) return 0;
    const int width = image.width(), height = image.height();
    const QPointF coordinate = imageCoordinate(direction.normalized(), width, height);
    const int x0 = int(std::floor(coordinate.x()));
    const int y0 = std::clamp(int(std::floor(coordinate.y())), 0, height - 1);
    const int y1 = std::clamp(int(std::floor(coordinate.y())) + 1, 0, height - 1);
    const double tx = coordinate.x() - std::floor(coordinate.x());
    const double ty = std::clamp(coordinate.y() - std::floor(coordinate.y()), 0.0, 1.0);
    const QRgb a = pixelAt(image, wrappedX(x0, width), y0);
    const QRgb b = pixelAt(image, wrappedX(x0 + 1, width), y0);
    const QRgb c = pixelAt(image, wrappedX(x0, width), y1);
    const QRgb d = pixelAt(image, wrappedX(x0 + 1, width), y1);
    const auto bilinear = [&](int v00, int v10, int v01, int v11) {
        return int(std::lround((1 - ty) * ((1 - tx) * v00 + tx * v10) +
                               ty * ((1 - tx) * v01 + tx * v11)));
    };
    return qRgba(bilinear(qRed(a), qRed(b), qRed(c), qRed(d)),
                 bilinear(qGreen(a), qGreen(b), qGreen(c), qGreen(d)),
                 bilinear(qBlue(a), qBlue(b), qBlue(c), qBlue(d)),
                 bilinear(qAlpha(a), qAlpha(b), qAlpha(c), qAlpha(d)));
}

void recompose(QImage &composite, const QImage &base, const QImage &layer) {
    composite = QImage(base.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < base.height(); ++y) {
        uchar *outRow = composite.scanLine(y);
        for (int x = 0; x < base.width(); ++x) {
            writePixel(outRow + 4 * x, over(pixelAt(layer, x, y), pixelAt(base, x, y)));
        }
    }
}

QRegion applyDab(QImage &layer, QImage &composite, const QImage &base,
                 const QImage *sampleImage, const QVector3D &center,
                 double radiusDegrees, Tool tool, const QColor &color,
                 const QQuaternion &sourceRotation) {
    if (base.isNull() || layer.size() != base.size() || composite.size() != base.size()) return {};
    if (tool == Tool::CloneStamp && (!sampleImage || sampleImage->size() != base.size())) return {};

    const int width = base.width(), height = base.height();
    const QVector3D normalized = center.normalized();
    const float longitude = std::atan2(normalized.x(), -normalized.z());
    const float latitude = std::asin(clampUnit(normalized.y()));
    const float radius = float(std::clamp(radiusDegrees, 0.01, 30.0)) * pi / 180.0f;
    const float cosRadius = std::cos(radius);
    const float sinLatitude = std::sin(latitude), cosLatitude = std::cos(latitude);
    const int cy = std::clamp(int((0.5f - latitude / pi) * height), 0, height - 1);
    const int ry = int(std::ceil(radius * height / pi)) + 2;
    const int y0 = std::max(0, cy - ry), y1 = std::min(height - 1, cy + ry);
    QRegion dirty;

    for (int y = y0; y <= y1; ++y) {
        const float phi = pi * (0.5f - float(y + 0.5f) / height);
        const float sinPhi = std::sin(phi), cosPhi = std::cos(phi);
        const float denominator = cosPhi * cosLatitude;
        const float threshold = denominator > 1e-7f
            ? (cosRadius - sinPhi * sinLatitude) / denominator
            : (cosRadius <= sinPhi * sinLatitude ? -1.0f : 2.0f);
        if (threshold > 1.0f) continue;
        const float span = threshold <= -1.0f ? pi : std::acos(clampUnit(threshold));
        const int cx = int((longitude / (2 * pi) + 0.5f) * width);
        const int rx = int(std::ceil(span * width / (2 * pi))) + 1;
        const int first = rx >= width / 2 ? 0 : cx - rx;
        const int last = rx >= width / 2 ? width - 1 : cx + rx;
        int minChanged = width, maxChanged = -1;
        uchar *layerRow = layer.scanLine(y);
        uchar *outRow = composite.scanLine(y);

        for (int xi = first; xi <= last; ++xi) {
            const int x = wrappedX(xi, width);
            const QVector3D pixelDirection = SphericalRaster::pixelDirection(x, y, width, height);
            if (QVector3D::dotProduct(pixelDirection, normalized) < cosRadius) continue;

            uchar *layerPixel = layerRow + 4 * x;
            QRgb nextLayer = 0;
            if (tool == Tool::Eraser) {
                nextLayer = qRgba(0, 0, 0, 0);
            } else if (tool == Tool::CloneStamp) {
                const QRgb sampled = sampleDirection(*sampleImage, sourceRotation.rotatedVector(pixelDirection));
                nextLayer = sampled;
            } else {
                nextLayer = over(color.rgba(), pixelAt(layer, x, y));
            }
            if (nextLayer == pixelAt(layer, x, y)) continue;
            writePixel(layerPixel, nextLayer);
            writePixel(outRow + 4 * x, over(nextLayer, pixelAt(base, x, y)));
            minChanged = std::min(minChanged, x);
            maxChanged = std::max(maxChanged, x);
        }
        if (maxChanged >= minChanged) dirty += QRect(minChanged, y, maxChanged - minChanged + 1, 1);
    }
    return dirty;
}

} // namespace SphericalRaster
