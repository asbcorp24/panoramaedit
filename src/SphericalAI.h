#pragma once

#include <QImage>
#include <QPoint>
#include <QRegion>
#include <QVector3D>
#include <optional>

namespace SphericalAI {

// A seam-aware rectangular crop. origin.x() is deliberately unwrapped;
// source pixels are sampled cyclically across the equirectangular seam.
struct Patch {
    QImage image;
    QImage mask;
    QPoint origin;
};

QRegion paintMask(QImage &mask, const QVector3D &center, double radiusDegrees);

// Returns nullopt for empty selections or selections larger than maxDimension.
// Small patches run through local models instead of processing all 8K/16K pixels.
std::optional<Patch> preparePatch(const QImage &source, const QImage &mask,
                                  int padding = 48, int maxDimension = 1024);

// Apply AI output only where the spherical selection mask is nonzero.
QRegion applyPatch(QImage &layer, QImage &composite, const Patch &patch,
                   const QImage &output, double opacity = 1.0);

// Feather an existing longitude mismatch symmetrically around 0°/360°.
QRegion smartSeam(QImage &layer, QImage &composite, int halfWidthPixels, double strength);
QRegion smoothSeam(QImage &layer, QImage &composite, int halfWidthPixels);

} // namespace SphericalAI
