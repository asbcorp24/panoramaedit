#pragma once
#include <QImage>
#include <QRegion>
#include <QVector3D>

namespace SphericalLiquify {
enum class Tool { Smudge, Push, Bloat, Pinch };
// Source may be the current composite itself: each dab samples pixels before
// committing its changes. Successive dabs accumulate, without full image
// copies. The base layer is untouched; longitude wraps across the seam.
QRegion apply(QImage &layer, QImage &composite, const QImage &source,
              const QVector3D &center, const QVector3D &previous,
              double radiusDegrees, double strength, Tool tool);
}
