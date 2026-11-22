#include "MainWindow.h"
#include "AiEngine.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSurfaceFormat>
#include <cstdio>

int main(int argc, char *argv[]) {
    // Native model check for setup_ai_windows.bat. Works without a GUI and
    // avoids launching Python just to validate an ONNX graph.
    if (argc >= 2 && QByteArray(argv[1]) == "--check-ai") {
        QCoreApplication app(argc, argv);
        const QStringList args = app.arguments();
        if (args.size() != 4 || (args[2] != "sharpen" && args[2] != "inpaint"))
            return 2;
        const auto mode = args[2] == "sharpen"
            ? AiEngine::Mode::Sharpen : AiEngine::Mode::Inpaint;
        const QString error = AiEngine::validate(mode, args[3]);
        if (!error.isEmpty()) {
            std::fprintf(stderr, "%s\n", error.toUtf8().constData());
            return 1;
        }
        return 0;
    }
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    app.setApplicationName("PanoramaEdit");
    app.setOrganizationName("PanoramaEdit");
    MainWindow window;
    window.show();
    return app.exec();
}
