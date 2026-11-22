#include "../src/SphericalRaster.h"

#include <QImage>
#include <QQuaternion>
#include <QVector3D>
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

    std::cout << "All spherical raster tests passed.\n";
    return EXIT_SUCCESS;
}
