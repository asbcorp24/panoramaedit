#include "../src/SphericalAI.h"
#include "../src/SphericalRaster.h"

#include <QColor>
#include <QImage>
#include <cstdlib>
#include <iostream>

namespace {
constexpr int w = 720, h = 360;
void require(bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
QImage image(QImage::Format format, QRgb color = qRgba(0, 0, 0, 255)) {
    QImage img(w, h, format);
    img.fill(color);
    return img;
}
} // namespace

int main() {
    using namespace SphericalAI;
    QImage source = image(QImage::Format_RGBA8888);
    QImage mask = image(QImage::Format_Grayscale8, qRgb(0, 0, 0));
    require(!preparePatch(source, mask), "empty selection yielded a crop");

    const QVector3D seam = SphericalRaster::pixelDirection(0, h / 2, w, h);
    const QRegion marked = paintMask(mask, seam, 3.0);
    require(!marked.isEmpty(), "AI selection drew nothing");
    require(mask.constScanLine(h / 2)[0] > 100, "AI brush missed left seam");
    require(mask.constScanLine(h / 2)[w - 1] > 100, "AI brush missed right seam");
    require(mask.constScanLine(h / 2)[w / 2] == 0, "AI brush hit opposite hemisphere");

    const auto patch = preparePatch(source, mask);
    require(patch.has_value(), "cannot crop seam-crossing AI mask");
    require(patch->image.width() < 200, "seam mask extracted almost entire panorama");
    require(patch->image.size() == patch->mask.size(), "AI image/mask sizes differ");
    QImage generated(patch->image.size(), QImage::Format_RGBA8888);
    generated.fill(Qt::green);
    QImage overlay = image(QImage::Format_RGBA8888, qRgba(0, 0, 0, 0));
    QImage composite = source;
    const QRegion changed = applyPatch(overlay, composite, *patch, generated);
    require(!changed.isEmpty(), "AI output not applied");
    require(qGreen(composite.pixel(0, h / 2)) > 100, "AI output missed left seam");
    require(qGreen(composite.pixel(w - 1, h / 2)) > 100, "AI output missed right seam");
    require(qGreen(composite.pixel(w / 2, h / 2)) == 0,
            "AI output changed outside the selected mask");
    QImage recomposed;
    SphericalRaster::recompose(recomposed, source, overlay);
    require(recomposed == composite, "AI overlay is inconsistent with undo/recompose");

    QImage polarMask = image(QImage::Format_Grayscale8, qRgb(0, 0, 0));
    const auto north = SphericalRaster::pixelDirection(200, 0, w, h);
    paintMask(polarMask, north, 4.0);
    require(polarMask.constScanLine(0)[200] > 0, "AI brush missed north pole");
    require(polarMask.constScanLine(h - 1)[200] == 0, "AI brush reached south pole");

    QImage mismatched = image(QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y) {
        mismatched.setPixelColor(0, y, QColor(Qt::red));
        mismatched.setPixelColor(w - 1, y, QColor(Qt::blue));
    }
    QImage seamLayer = image(QImage::Format_RGBA8888, qRgba(0, 0, 0, 0));
    QImage seamComposite = mismatched;
    const QRegion fixed = smoothSeam(seamLayer, seamComposite, 10);
    require(!fixed.isEmpty(), "seam repair changed nothing");
    require(seamComposite.pixel(0, h / 2) == seamComposite.pixel(w - 1, h / 2),
            "seam repair did not make both edges identical");
    require(seamComposite.pixel(w / 2, h / 2) == mismatched.pixel(w / 2, h / 2),
            "seam repair damaged the middle of the panorama");
    SphericalRaster::recompose(recomposed, mismatched, seamLayer);
    require(recomposed == seamComposite, "seam repair does not survive recomposition");

    std::cout << "Offline AI mask and spherical seam tests passed.\n";
    return EXIT_SUCCESS;
}
