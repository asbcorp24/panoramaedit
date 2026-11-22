#pragma once

#include <QImage>
#include <QString>
#include <atomic>
#include <memory>

namespace AiEngine {

enum class Mode { Sharpen, Inpaint };

struct Result {
    QImage image;
    QString error;
    bool usedGpu = false;
};

// Runs entirely in native C++ with ONNX Runtime (DirectML, then CPU fallback).
// Call on a worker thread, never from the Qt GUI thread.
Result run(Mode mode, const QString &modelPath, const QImage &image,
           const QImage &mask, const std::shared_ptr<std::atomic_bool> &canceled = {});

// Used by the model-only Windows setup script (without Python).
QString validate(Mode mode, const QString &modelPath);

} // namespace AiEngine
