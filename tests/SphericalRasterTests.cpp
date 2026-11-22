#include "../src/SphericalRaster.h"
#include "../src/SphericalLiquify.h"

#include <QImage>
#include <QQuaternion>
#include <QVector3D>
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
constexpr int width = 720;
constexpr int height = 360;

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

QImage blankLayer() {
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    return image;
}
}

int main() {
    using namespace SphericalRaster;

    QImage base(width, height, QImage::Format_RGBA8888);
    base.fill(Qt::black);
    auto layer = blankLayer();
    QImage composite = base;

    // A brush near the 360° seam must paint BOTH ends of the source raster.
    const auto nearSeam = pixelDirection(0, height / 2, width, height);
    const auto strokeRegion = applyDab(layer, composite, base, nullptr, nearSeam,
                                       3.0, Tool::Brush, QColor(Qt::red));
    require(!strokeRegion.isEmpty(), "geodesic brush changed no pixels");
    require(qRed(composite.pixel(0, height / 2)) == 255, "brush missed left seam");
    require(qRed(composite.pixel(width - 1, height / 2)) == 255, "brush missed right seam");
    require(qRed(composite.pixel(width / 2, height / 2)) == 0, "brush painted opposite hemisphere");

    // Erasing the overlay restores the exact base pixel, even across the seam.
    applyDab(layer, composite, base, nullptr, nearSeam, 3.0, Tool::Eraser, Qt::black);
    require(composite.pixel(0, height / 2) == base.pixel(0, height / 2), "erase missed left seam");
    require(composite.pixel(width - 1, height / 2) == base.pixel(width - 1, height / 2), "erase missed right seam");

    // Clone samples from another spherical direction via 3D rotation.
    for (int y = 0; y < height; ++y)
        for (int x = width / 2; x < width; ++x)
            base.setPixelColor(x, y, QColor(Qt::blue));
    layer = blankLayer();
    composite = base;
    const QImage frozenSource = composite;
    const auto target = pixelDirection(180, height / 2, width, height);
    const auto source = pixelDirection(550, height / 2, width, height);
    const auto rotation = QQuaternion::rotationTo(target, source);
    applyDab(layer, composite, base, &frozenSource, target, 3.0, Tool::CloneStamp, Qt::white, rotation);
    require(qBlue(composite.pixel(180, height / 2)) == 255, "clone did not copy source hemisphere");
    require(qBlue(composite.pixel(180, height / 2)) != qBlue(base.pixel(180, height / 2)),
            "clone modified source instead of destination");

    // North-pole dabs are valid and must not change the antipodal south pole.
    layer = blankLayer();
    composite = base;
    applyDab(layer, composite, base, nullptr, pixelDirection(200, 0, width, height),
             4.0, Tool::Brush, QColor(Qt::green));
    require(qGreen(composite.pixel(200, 0)) == 255, "north pole brush failed");
    require(qGreen(composite.pixel(200, height - 1)) == 0, "north pole brush reached south pole");

    // Liquify now samples the live composite, never a full copy. Repeated
    // bloat/pinch dabs must ACCUMULATE rather than spring back to the source.
    QImage gradient(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int red = std::min(255, std::abs(x - width / 2) * 10);
            gradient.setPixelColor(x, y, QColor(red, 0, 0));
        }
    }
    const int sampleX = width / 2 + 16, sampleY = height / 2;
    const QVector3D warpCenter = pixelDirection(width / 2, sampleY, width, height);
    layer = blankLayer();
    composite = gradient;
    const int initialRed = qRed(composite.pixel(sampleX, sampleY));
    require(!SphericalLiquify::apply(layer, composite, composite, warpCenter, warpCenter,
        12.0, 1.0, SphericalLiquify::Tool::Bloat).isEmpty(), "bloat did nothing");
    const int firstBloat = qRed(composite.pixel(sampleX, sampleY));
    SphericalLiquify::apply(layer, composite, composite, warpCenter, warpCenter,
        12.0, 1.0, SphericalLiquify::Tool::Bloat);
    const int secondBloat = qRed(composite.pixel(sampleX, sampleY));
    require(firstBloat < initialRed && secondBloat < firstBloat,
            "repeated bloat should expand further, not reset");
    require(qRed(gradient.pixel(sampleX, sampleY)) == initialRed,
            "liquify modified its source/base panorama");

    layer = blankLayer();
    composite = gradient;
    SphericalLiquify::apply(layer, composite, composite, warpCenter, warpCenter,
        12.0, 1.0, SphericalLiquify::Tool::Pinch);
    const int firstPinch = qRed(composite.pixel(sampleX, sampleY));
    SphericalLiquify::apply(layer, composite, composite, warpCenter, warpCenter,
        12.0, 1.0, SphericalLiquify::Tool::Pinch);
    const int secondPinch = qRed(composite.pixel(sampleX, sampleY));
    require(firstPinch > initialRed && secondPinch > firstPinch,
            "repeated pinch should contract further, not reset");

    // A push across the seam must sample from the previous cursor position;
    // the finger blends dragged pixels, unlike the stronger geometric push.
    QImage seamSource(width, height, QImage::Format_RGBA8888);
    seamSource.fill(Qt::black);
    for (int y = height / 2 - 15; y <= height / 2 + 15; ++y)
        for (int x = width - 12; x < width; ++x)
            seamSource.setPixelColor(x, y, QColor(Qt::red));
    const QVector3D seamDest = pixelDirection(2, height / 2, width, height);
    const QVector3D seamPrev = pixelDirection(width - 4, height / 2, width, height);
    layer = blankLayer();
    composite = seamSource;
    const QRegion seamRegion = SphericalLiquify::apply(layer, composite, composite,
        seamDest, seamPrev, 12.0, 1.0, SphericalLiquify::Tool::Push);
    require(!seamRegion.isEmpty(), "seam push changed nothing");
    for (const QRect &rect : seamRegion)
        require(rect.width() < width / 2, "seam push uploads an entire row");
    const int pushedRed = qRed(composite.pixel(2, height / 2));
    require(pushedRed > 100, "seam push did not move pixels across boundary");
    require(qRed(composite.pixel(width / 2, height / 2)) == 0,
            "seam push affected opposite longitude");

    // Reading from an independent source copy must yield the same one-dab
    // result as reading the live composite (no scan-order feedback).
    const QImage referenceSource = seamSource.copy();
    QImage referenceComposite = seamSource;
    QImage referenceLayer = blankLayer();
    SphericalLiquify::apply(referenceLayer, referenceComposite, referenceSource,
        seamDest, seamPrev, 12.0, 1.0, SphericalLiquify::Tool::Push);
    require(referenceComposite == composite, "liquify depends on in-place write order");

    layer = blankLayer();
    composite = seamSource;
    SphericalLiquify::apply(layer, composite, composite, seamDest, seamPrev,
        12.0, 1.0, SphericalLiquify::Tool::Smudge);
    require(qRed(composite.pixel(2, height / 2)) > 0 &&
            qRed(composite.pixel(2, height / 2)) < pushedRed,
            "finger should blend rather than duplicate push");

    QImage polarGradient(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x)
            polarGradient.setPixelColor(x, y, QColor(std::min(y * 24, 255), 0, 0));
    }
    layer = blankLayer();
    composite = polarGradient;
    const auto north = pixelDirection(200, 0, width, height);
    const auto northRegion = SphericalLiquify::apply(layer, composite, composite,
        north, north, 8.0, 1.0, SphericalLiquify::Tool::Bloat);
    require(!northRegion.isEmpty(), "liquify at north pole changed nothing");
    require(composite.pixel(200, height - 1) == polarGradient.pixel(200, height - 1),
            "north-pole liquify reached the south pole");

    std::cout << "All spherical raster and cumulative liquify tests passed.\n";
    return EXIT_SUCCESS;
}
