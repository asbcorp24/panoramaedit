#include "AiEngine.h"

#include <QFileInfo>
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>
#include <mutex>
#endif

namespace {

bool isCanceled(const std::shared_ptr<std::atomic_bool> &flag) {
    return flag && flag->load(std::memory_order_relaxed);
}

int reflected(int index, int length) {
    if (length <= 1) return 0;
    while (index < 0 || index >= length) {
        if (index < 0) index = -index;
        else index = 2 * length - index - 2;
    }
    return index;
}

#ifdef _WIN32
struct CachedSession {
    QString path;
    QDateTime modified;
    qint64 bytes = 0;
    bool gpu = false;
    std::unique_ptr<Ort::Session> session;
};

struct Runtime {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "PanoramaEdit"};
    CachedSession sharpen;
    CachedSession inpaint;
    std::mutex mutex;
};

Runtime &runtime() {
    static Runtime state;
    return state;
}

Ort::SessionOptions options(bool gpu) {
    Ort::SessionOptions setting;
    setting.DisableMemPattern(); // Required for DirectML.
    setting.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    setting.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    setting.SetLogSeverityLevel(3);
    if (gpu) {
        OrtStatus *status = OrtSessionOptionsAppendExecutionProvider_DML(setting, 0);
        if (status) {
            const std::string message = Ort::GetApi().GetErrorMessage(status);
            Ort::GetApi().ReleaseStatus(status);
            throw std::runtime_error(message);
        }
    }
    return setting;
}

void openSession(Runtime &context, CachedSession &cached,
                 const QString &path, const QFileInfo &info, bool gpu) {
    // Invalidate the old session before loading a replaced ONNX file.
    cached.session.reset();
    const std::wstring nativePath = path.toStdWString();
    Ort::SessionOptions setting = options(gpu);
    cached.session = std::make_unique<Ort::Session>(
        context.env, nativePath.c_str(), setting);
    cached.path = path;
    cached.modified = info.lastModified();
    cached.bytes = info.size();
    cached.gpu = gpu;
}

void ensureSession(Runtime &context, CachedSession &cached,
                   const QString &path, const QFileInfo &info) {
    if (cached.session && cached.path == path && cached.bytes == info.size() &&
        cached.modified == info.lastModified()) return;
    try {
        openSession(context, cached, path, info, true);
    } catch (const std::exception &) {
        // GPU, driver and unsupported-op failures must not disable offline AI.
        openSession(context, cached, path, info, false);
    }
}

struct TensorInput {
    std::vector<float> data;
    std::vector<int64_t> shape;
};

TensorInput rgbTensor(const QImage &image, int height, int width,
                      bool reflectPadding) {
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    TensorInput input;
    input.shape = {1, 3, height, width};
    input.data.resize(size_t(3) * height * width);
    for (int y = 0; y < height; ++y) {
        const int sy = reflectPadding ? reflected(y, rgb.height()) : y;
        const uchar *scanline = rgb.constScanLine(sy);
        for (int x = 0; x < width; ++x) {
            const int sx = reflectPadding ? reflected(x, rgb.width()) : x;
            const uchar *pixel = scanline + 3 * sx;
            for (int channel = 0; channel < 3; ++channel)
                input.data[(size_t(channel) * height + y) * width + x] =
                    float(pixel[channel]) / 255.0f;
        }
    }
    return input;
}

TensorInput maskTensor(const QImage &mask) {
    const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8)
        .scaled(512, 512, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    TensorInput input;
    input.shape = {1, 1, 512, 512};
    input.data.resize(512 * 512);
    for (int y = 0; y < 512; ++y) {
        const uchar *scanline = gray.constScanLine(y);
        for (int x = 0; x < 512; ++x)
            input.data[size_t(y) * 512 + x] = scanline[x] >= 128 ? 1.0f : 0.0f;
    }
    return input;
}

QImage decodeOutput(const Ort::Value &tensor, const QSize &sourceSize,
                    bool sharpen) {
    if (!tensor.IsTensor()) throw std::runtime_error("Model did not return a tensor");
    const auto shape = tensor.GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 4 || shape[0] != 1 || shape[1] != 3 ||
        shape[2] <= 0 || shape[3] <= 0)
        throw std::runtime_error("Unexpected ONNX output tensor dimensions");
    if (tensor.GetTensorTypeAndShapeInfo().GetElementType() !=
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("Expected float32 ONNX model output");
    const int height = int(shape[2]);
    const int width = int(shape[3]);
    const float *data = tensor.GetTensorData<float>();
    const size_t plane = size_t(height) * width;
    float maximum = 0.0f;
    if (sharpen) {
        for (size_t i = 0; i < 3 * plane; ++i)
            if (std::isfinite(data[i])) maximum = std::max(maximum, data[i]);
    }
    const float multiplier = sharpen && maximum <= 2.0f ? 255.0f : 1.0f;
    QImage result(width, height, QImage::Format_RGBA8888);
    if (result.isNull()) throw std::runtime_error("Not enough RAM for ONNX output image");
    for (int y = 0; y < height; ++y) {
        uchar *scanline = result.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const size_t pos = size_t(y) * width + x;
            for (int channel = 0; channel < 3; ++channel) {
                const float raw = data[size_t(channel) * plane + pos] * multiplier;
                scanline[4 * x + channel] = uchar(std::lround(
                    std::clamp(std::isfinite(raw) ? raw : 0.0f, 0.0f, 255.0f)));
            }
            scanline[4 * x + 3] = 255;
        }
    }
    if (result.size() != sourceSize) {
        // Dynamic NAFNet pads at the bottom/right; first discard the padding.
        if (sharpen && width >= sourceSize.width() && height >= sourceSize.height())
            result = result.copy(0, 0, sourceSize.width(), sourceSize.height());
        else
            result = result.scaled(sourceSize, Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
    }
    return result;
}

QImage infer(Ort::Session &session, AiEngine::Mode mode, const QImage &source,
             const QImage &mask, const std::shared_ptr<std::atomic_bool> &cancel) {
    Ort::AllocatorWithDefaultOptions allocator;
    const size_t count = session.GetInputCount();
    std::vector<std::string> names;
    names.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto name = session.GetInputNameAllocated(i, allocator);
        names.emplace_back(name.get());
    }
    TensorInput color, selection;
    if (mode == AiEngine::Mode::Sharpen) {
        if (count != 1) throw std::runtime_error("NAFNet must have exactly one input");
        const auto shape = session.GetInputTypeInfo(0)
            .GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 4 ||
            session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetElementType() !=
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("Incorrect NAFNet model: expected float32 BCHW");
        const bool fixedH = shape[2] > 0;
        const bool fixedW = shape[3] > 0;
        const int height = fixedH ? int(shape[2]) : (source.height() + 31) / 32 * 32;
        const int width = fixedW ? int(shape[3]) : (source.width() + 31) / 32 * 32;
        const bool resized = (fixedH && height != source.height()) ||
                             (fixedW && width != source.width());
        const QImage input = resized
            ? source.scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            : source;
        color = rgbTensor(input, height, width, !resized);
    } else {
        if (count != 2) throw std::runtime_error("LaMa must have image/mask inputs");
        const QImage scaled = source.scaled(512, 512, Qt::IgnoreAspectRatio,
                                           Qt::SmoothTransformation);
        color = rgbTensor(scaled, 512, 512, false);
        selection = maskTensor(mask);
    }
    if (isCanceled(cancel)) return {};

    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value colorValue = Ort::Value::CreateTensor<float>(
        memory, color.data.data(), color.data.size(),
        color.shape.data(), color.shape.size());
    Ort::Value maskValue;
    if (mode == AiEngine::Mode::Inpaint)
        maskValue = Ort::Value::CreateTensor<float>(memory, selection.data.data(),
            selection.data.size(), selection.shape.data(), selection.shape.size());

    std::vector<Ort::Value> values;
    std::vector<const char *> inputNames;
    for (const auto &name : names) {
        inputNames.push_back(name.c_str());
        if (mode == AiEngine::Mode::Sharpen || name == "image")
            values.push_back(std::move(colorValue));
        else if (name == "mask")
            values.push_back(std::move(maskValue));
        else throw std::runtime_error("Unknown LaMa input: " + name);
    }
    const auto outputName = session.GetOutputNameAllocated(0, allocator);
    const char *outputNames[] = {outputName.get()};
    auto outputs = session.Run(Ort::RunOptions{nullptr}, inputNames.data(),
                               values.data(), values.size(), outputNames, 1);
    if (isCanceled(cancel)) return {};
    return decodeOutput(outputs.at(0), source.size(), mode == AiEngine::Mode::Sharpen);
}
#endif

} // namespace

namespace AiEngine {
QString validate(Mode mode, const QString &modelPath) {
    const QFileInfo file(modelPath);
    if (!file.isFile()) return QStringLiteral("Не найдена модель: %1").arg(modelPath);
#ifdef _WIN32
    try {
        Runtime &context = runtime();
        std::lock_guard lock(context.mutex);
        CachedSession &cached = mode == Mode::Sharpen ? context.sharpen : context.inpaint;
        ensureSession(context, cached, modelPath, file);
        if (mode == Mode::Sharpen && cached.session->GetInputCount() != 1)
            throw std::runtime_error("NAFNet expects one RGB input");
        if (mode == Mode::Inpaint && cached.session->GetInputCount() != 2)
            throw std::runtime_error("LaMa expects image and mask inputs");
        return {};
    } catch (const std::exception &error) {
        return QStringLiteral("ONNX Runtime: %1").arg(QString::fromLocal8Bit(error.what()));
    }
#else
    Q_UNUSED(mode)
    return QStringLiteral("Проверка ONNX доступна только для Windows");
#endif
}

Result run(Mode mode, const QString &modelPath, const QImage &image,
           const QImage &mask, const std::shared_ptr<std::atomic_bool> &canceled) {
    if (image.isNull() || mask.isNull() || image.size() != mask.size())
        return {{}, QStringLiteral("Некорректное изображение или маска выделения")};
    QFileInfo model(modelPath);
    if (!model.isFile())
        return {{}, QStringLiteral("Модель не найдена: %1. Запустите setup_ai_windows.bat")
                        .arg(modelPath)};
#ifdef _WIN32
    try {
        Runtime &context = runtime();
        std::lock_guard lock(context.mutex); // only one GPU inference at a time
        CachedSession &cached = mode == Mode::Sharpen ? context.sharpen : context.inpaint;
        ensureSession(context, cached, modelPath, model);
        try {
            QImage output = infer(*cached.session, mode, image, mask, canceled);
            return {std::move(output), {}, cached.gpu};
        } catch (const Ort::Exception &) {
            if (!cached.gpu) throw;
            // DirectML can load a graph yet fail running a particular tensor.
            openSession(context, cached, modelPath, model, false);
            QImage output = infer(*cached.session, mode, image, mask, canceled);
            return {std::move(output), {}, false};
        }
    } catch (const std::exception &error) {
        const QByteArray description(error.what());
        QString message = QString::fromUtf8(description);
        if (message.contains(QChar::ReplacementCharacter))
            message = QString::fromLocal8Bit(description);
        return {{}, QStringLiteral("Ошибка ONNX Runtime: %1").arg(message)};
    }
#else
    Q_UNUSED(mode)
    Q_UNUSED(canceled)
    return {{}, QStringLiteral("Встроенный ИИ поддерживается в Windows-сборке")};
#endif
}
} // namespace AiEngine
