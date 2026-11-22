#pragma once

#include <QColor>
#include <QImage>
#include <QQuaternion>
#include <QRegion>
#include <QVector3D>

namespace SphericalRaster {

enum class Tool { Brush, Eraser, CloneStamp };

// Pixels are top-to-bottom equirectangular. Longitude wraps at x=0/width.
QVector3D pixelDirection(int x, int y, int width, int height);
QRgb sampleDirection(const QImage &image, const QVector3D &direction);

// Paint one geodesic circular dab into the single transparent edit layer.
// The full-resolution base image is never overwritten.
// For clone stamping, sourceRotation maps a destination direction to the
// corresponding source direction, and sampleImage is frozen per stroke.
QRegion applyDab(QImage &layer, QImage &composite, const QImage &base,
                 const QImage *sampleImage, const QVector3D &center,
                 double radiusDegrees, Tool tool, const QColor &color,
                 const QQuaternion &sourceRotation = QQuaternion());

void recompose(QImage &composite, const QImage &base, const QImage &layer);

} // namespace SphericalRaster
