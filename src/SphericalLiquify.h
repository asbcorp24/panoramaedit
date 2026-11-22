#pragma once
#include <QImage>
#include <QRegion>
#include <QVector3D>

namespace SphericalLiquify {
enum class Tool { Smudge, Push, Bloat, Pinch };
// Source is a frozen snapshot from the start of the stroke. The resulting
// pixels are written to the transparent edit layer (base remains untouched).
// Directions are in world/sphere coordinates; the X seam wraps automatically.
QRegion apply(QImage &layer, QImage &composite, const QImage &source,
              const QVector3D &center, const QVector3D &previous,
              double radiusDegrees, double strength, Tool tool);
}
