#include "../src/AiEngine.h"

#include <QCoreApplication>
#include <QColor>
#include <QImage>
#include <QDebug>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QImage image(65, 33, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(x * 3, y * 7, 120));
    QImage mask(image.size(), QImage::Format_Grayscale8);
    mask.fill(255);

    // A missing weight must produce an actionable error, not terminate the app.
    if (AiEngine::validate(AiEngine::Mode::Sharpen,
                           "model-is-deliberately-not-present.onnx").isEmpty()) return 1;
    if (AiEngine::run(AiEngine::Mode::Sharpen,
                      "model-is-deliberately-not-present.onnx", image, mask).error.isEmpty()) return 2;

    if (app.arguments().size() > 1) {
        const QString model = app.arguments().at(1);
        const QString error = AiEngine::validate(AiEngine::Mode::Sharpen, model);
        if (!error.isEmpty()) {
            qCritical() << "Native NAFNet model validation failed:" << error;
            return 3;
        }
        const auto result = AiEngine::run(AiEngine::Mode::Sharpen, model, image, mask);
        if (!result.error.isEmpty() || result.image.size() != image.size()) {
            qCritical() << "Native inference failed:" << result.error << result.image.size();
            return 4;
        }
        qInfo() << "Native NAFNet inference OK, 65x33, GPU:" << result.usedGpu;
    }
    return 0;
}
