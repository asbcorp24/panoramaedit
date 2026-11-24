#pragma once

#include <QImage>
#include <QString>
#include <atomic>
#include <memory>

namespace AiEngine {

// SAM-compatible combined ONNX graph: image [1,3,1024,1024], point_coords
// [1,1,2], point_labels [1,1], masks [1,1,H,W] (logits).
enum class Mode { Sharpen, Inpaint };

enum class Phase { Loading, Processing, Blending, Finished };

// Written on the worker thread and read on the GUI thread. `completed/total`
// always counts whole, actually finished inference tiles (never simulated).
struct Progress {
    std::atomic<Phase> phase{Phase::Loading};
    std::atomic<int> total{0};
    std::atomic<int> completed{0};
    std::atomic<bool> usingGpu{false};
};

struct Result {
    QImage image;
    QString error;
    bool usedGpu = false;
};

Result segment(const QString &modelPath, const QImage &image);

// Runs entirely in native C++ with ONNX Runtime (DirectML, then CPU fallback).
// Call on a worker thread, never from the Qt GUI thread.
Result run(Mode mode, const QString &modelPath, const QImage &image,
           const QImage &mask, const std::shared_ptr<std::atomic_bool> &canceled = {},
           const std::shared_ptr<Progress> &progress = {});

// Used by the model-only Windows setup script (without Python).
QString validate(Mode mode, const QString &modelPath);

} // namespace AiEngine
