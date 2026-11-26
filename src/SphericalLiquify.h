#pragma once
#include <QImage>
#include <QRegion>
#include <QVector3D>

namespace SphericalLiquify {
enum class Tool { Smudge, Push, Bloat, Pinch, Stretch, Compress };
// Pull a spherical region from start to end. The influence falls off geodesically
// from the destination; longitude wraps and the poles use spherical sampling.
QRegion applyDrag(QImage &layer, QImage &composite, const QImage &source,
                  const QVector3D &start, const QVector3D &end,
                  double radiusDegrees, double strength);
// Source may be the current composite itself: each dab samples pixels before
// committing its changes. Successive dabs accumulate, without full image
// copies. The base layer is untouched; longitude wraps across the seam.
QRegion apply(QImage &layer, QImage &composite, const QImage &source,
              const QVector3D &center, const QVector3D &previous,
              double radiusDegrees, double strength, Tool tool);
}
