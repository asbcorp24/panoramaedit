#include "SphereView.h"
#include "SphericalLiquify.h"
#include <QMouseEvent>
#include <QImageReader>
#include <QFile>
#include <webp/decode.h>
#include <QEnterEvent>
#include <QWheelEvent>
#include <QOpenGLContext>
#include <QMatrix4x4>
#include <cmath>
#include <algorithm>
#include <vector>
#include <QPainter>

namespace {
constexpr float PI = 3.14159265358979323846f;
constexpr float rad = PI / 180.0f;
float clampf(float x, float a, float b) { return std::clamp(x, a, b); }
QVector3D sphereDir(float longitude, float latitude) {
    return QVector3D(std::cos(latitude) * std::sin(longitude),
                     std::sin(latitude),
                     -std::cos(latitude) * std::cos(longitude));
}
}

SphereView::SphereView(QWidget *parent) : QOpenGLWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}
SphereView::~SphereView() {
    if (context()) {
        makeCurrent();
        if (texture_) glDeleteTextures(1, &texture_);
        if (maskTexture_) glDeleteTextures(1, &maskTexture_);
        vertexArray_.destroy();
        vertexBuffer_.destroy();
        indexBuffer_.destroy();
        doneCurrent();
    }
}
void SphereView::setMode(Mode mode) {
    mode_ = mode;
    roundCenter_.reset(); roundContour_.clear();
    if (mode == Mode::RoundObject) emit statusMessage(tr("Округление: щёлкните центр, затем точки контура; Enter — применить."));
    setCursor(mode == Mode::Navigate ? Qt::OpenHandCursor : Qt::CrossCursor);
    update(); // Refresh the angular brush outline on tool switches.
    if (mode == Mode::CloneStamp)
        emit statusMessage(tr("Штамп: Ctrl + щелчок левой кнопкой выбирает источник, затем рисуйте."));
    if (mode == Mode::AiSharpen || mode == Mode::AiInpaint)
        emit statusMessage(tr("Выделите участок левой кнопкой, затем нажмите «Применить ИИ»."));
}
void SphereView::setBrushRadius(double degrees) {
    brushRadius_ = std::clamp(degrees, 0.1, 30.0);
    update(); // The outline follows manual radius input/slider immediately.
}
void SphereView::setModified(bool modified) {
    if (modified_ == modified) return;
    modified_ = modified;
    emit modifiedChanged(modified);
}
bool SphereView::openImage(const QString &path, QString *error) {
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    QImage loaded = reader.read();
    if (loaded.isNull()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && file.size() <= 512LL * 1024 * 1024) {
            const QByteArray bytes = file.readAll();
            const auto *data = reinterpret_cast<const uint8_t *>(bytes.constData());
            int w = 0, h = 0;
            if (WebPGetInfo(data, size_t(bytes.size()), &w, &h) &&
                w > 0 && h > 0 && qint64(w) * h <= 134217728) {
                QImage decoded(w, h, QImage::Format_RGBA8888);
                if (!decoded.isNull() && WebPDecodeRGBAInto(data, size_t(bytes.size()),
                    decoded.bits(), size_t(decoded.sizeInBytes()), decoded.bytesPerLine()))
                    loaded = decoded;
            }
        }
    }
    if (loaded.isNull()) {
        if (error) *error = tr("Не удалось прочитать изображение: %1\nПричина: %2\nПоддерживаемые форматы: %3").arg(path, reader.errorString(), QString::fromLatin1(QImageReader::supportedImageFormats().join(", ")));
        return false;
    }
    // Some panorama exporters round the width/height or retain a few extra
    // pixels. Keep the original image and map it over the whole sphere.
    // Only clearly non-panoramic aspect ratios are rejected.
    const double aspect = double(loaded.width()) / loaded.height();
    constexpr double expectedAspect = 2.0;
    constexpr double allowedRelativeDeviation = 0.05; // 5%
    if (std::abs(aspect / expectedAspect - 1.0) > allowedRelativeDeviation) {
        if (error) *error = tr("Ожидалась панорама с пропорцией около 2:1 (допуск 5%%), получено %1 × %2.")
            .arg(loaded.width()).arg(loaded.height());
        return false;
    }
    roundCenter_.reset(); roundContour_.clear();
    baseImage_ = loaded.convertToFormat(QImage::Format_RGBA8888);
    editLayer_ = QImage(baseImage_.size(), QImage::Format_RGBA8888);
    editLayer_.fill(Qt::transparent);
    composite_ = baseImage_;
    cloneSnapshot_ = {};
    aiMask_ = {};
    aiMaskDirty_ = {};
    hasAiSelection_ = false;
    fullMaskDirty_ = true;
    cloneSource_.reset();
    undoStack_.clear();
    redoStack_.clear();
    revision_ = savedRevision_ = 0;
    nextRevision_ = 1;
    ++documentId_;
    strokeHasChanged_ = false;
    yaw_ = pitch_ = 0;
    fullTextureDirty_ = true;
    partialTextureDirty_ = {};
    setModified(false);
    update();
    emit imageChanged();
    return true;
}
bool SphereView::saveImage(const QString &path) {
    if (composite_.isNull() || !composite_.save(path)) return false;
    savedRevision_ = revision_;
    setModified(false);
    return true;
}
void SphereView::pushUndo() {
    if (editLayer_.isNull()) return;
    if (undoStack_.size() >= historyLimit_) undoStack_.removeFirst();
    undoStack_.push_back({editLayer_, revision_});
    redoStack_.clear();
}
void SphereView::refreshComposite() {
    SphericalRaster::recompose(composite_, baseImage_, editLayer_);
    fullTextureDirty_ = true;
    partialTextureDirty_ = {};
    setModified(revision_ != savedRevision_);
    update();
}
void SphereView::undo() {
    if (undoStack_.empty() || editLayer_.isNull()) return;
    redoStack_.push_back({editLayer_, revision_});
    const auto prior = undoStack_.takeLast();
    editLayer_ = prior.layer;
    revision_ = prior.revision;
    refreshComposite();
}
void SphereView::redo() {
    if (redoStack_.empty() || editLayer_.isNull()) return;
    undoStack_.push_back({editLayer_, revision_});
    const auto later = redoStack_.takeLast();
    editLayer_ = later.layer;
    revision_ = later.revision;
    refreshComposite();
}
std::optional<SphericalAI::Patch> SphereView::prepareAiPatch() const {
    if (!hasAiSelection_) return std::nullopt;
    return SphericalAI::preparePatch(composite_, aiMask_);
}
void SphereView::clearAiSelection() {
    aiMask_ = {};
    aiMaskDirty_ = {};
    fullMaskDirty_ = true;
    hasAiSelection_ = false;
    update();
}
bool SphereView::applyAiResult(const SphericalAI::Patch &patch, const QImage &result) {
    if (!hasImage() || patch.image.isNull() || result.size() != patch.image.size())
        return false;
    pushUndo();
    const QRegion region = SphericalAI::applyPatch(editLayer_, composite_, patch,
        result.convertToFormat(QImage::Format_RGBA8888), toolStrength_);
    if (region.isEmpty()) {
        if (!undoStack_.isEmpty()) undoStack_.removeLast();
        return false;
    }
    partialTextureDirty_ += region;
    revision_ = nextRevision_++;
    setModified(revision_ != savedRevision_);
    clearAiSelection();
    update();
    return true;
}
void SphereView::smoothSeam() {
    if (!hasImage()) return;
    pushUndo();
    const int halfWidth = std::max(1, int(std::ceil(brushRadius_ * baseImage_.width() / 360.0)));
    const QRegion region = SphericalAI::smoothSeam(editLayer_, composite_, halfWidth);
    if (region.isEmpty()) {
        if (!undoStack_.isEmpty()) undoStack_.removeLast();
        emit statusMessage(tr("Шов уже совпадает — исправления не требуются."));
        return;
    }
    partialTextureDirty_ += region;
    revision_ = nextRevision_++;
    setModified(revision_ != savedRevision_);
    update();
    emit statusMessage(tr("Шов 0°/360° сглажен. Ctrl+Z — отменить."));
}
void SphereView::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.075f, 0.085f, 0.11f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    shader_ = std::make_unique<QOpenGLShaderProgram>();
    const char *vs = R"(#version 330 core
layout(location=0) in vec3 inPosition;
layout(location=1) in vec2 inUV;
out vec2 vUV;
out vec3 vDirection;
uniform mat4 uMVP;
void main() { vUV = inUV; vDirection = inPosition; gl_Position = uMVP * vec4(inPosition, 1.0); }
)";
    const char *fs = R"(#version 330 core
in vec2 vUV;
in vec3 vDirection;
out vec4 fragColor;
uniform sampler2D uPanorama;
uniform sampler2D uMask;
uniform bool uShowMask;
uniform bool uShowCursor;
uniform vec3 uCursorDirection;
uniform float uCursorRadius;
uniform float uCursorPixelAngle;
const float PI = 3.14159265358979323846;
void main() {
    vec3 d = normalize(vDirection);
    float u = 0.5 + atan(d.x, -d.z) / (2.0 * PI);
    // OpenGL treats the first row of QImage as the bottom texture row (v=0).
    float v = 0.5 - asin(clamp(d.y, -1.0, 1.0)) / PI;
    fragColor = texture(uPanorama, vec2(u, v));
    if (uShowMask) {
        float selected = texture(uMask, vec2(u, v)).r;
        fragColor.rgb = mix(fragColor.rgb, vec3(0.12, 0.65, 1.0), selected * 0.45);
    }
    if (uShowCursor) {
        // Same great-circle angular distance as the actual spherical brush.
        // The contour therefore wraps across 0/360 degrees and both poles.
        float angle = acos(clamp(dot(d, normalize(uCursorDirection)), -1.0, 1.0));
        float offset = abs(angle - uCursorRadius);
        float ringShadow = 1.0 - smoothstep(uCursorPixelAngle * 1.4,
                                             uCursorPixelAngle * 2.7, offset);
        float ring = 1.0 - smoothstep(uCursorPixelAngle * 0.45,
                                       uCursorPixelAngle * 1.4, offset);
        float inside = 1.0 - smoothstep(uCursorRadius - uCursorPixelAngle * 2.0,
                                         uCursorRadius, angle);
        fragColor.rgb = mix(fragColor.rgb, vec3(0.05, 0.32, 0.45), inside * 0.12);
        fragColor.rgb = mix(fragColor.rgb, vec3(0.02, 0.04, 0.08), ringShadow * 0.86);
        fragColor.rgb = mix(fragColor.rgb, vec3(0.15, 0.94, 1.0), ring * 0.94);
    }
}
)";
    if (!shader_->addShaderFromSourceCode(QOpenGLShader::Vertex, vs) ||
        !shader_->addShaderFromSourceCode(QOpenGLShader::Fragment, fs) ||
        !shader_->link()) {
        qFatal("Cannot initialize panorama OpenGL shader");
    }
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    constexpr int slices = 256, stacks = 128;
    for (int y = 0; y <= stacks; ++y) {
        const float v = float(y) / stacks;
        const float latitude = (0.5f - v) * PI;
        for (int x = 0; x <= slices; ++x) {
            const float u = float(x) / slices;
            const QVector3D d = sphereDir((u * 2.0f - 1.0f) * PI, latitude);
            // QImage rows are top-first; GL sees row 0 at texture coordinate v=0.
            vertices.insert(vertices.end(), {d.x(), d.y(), d.z(), u, v});
        }
    }
    for (int y = 0; y < stacks; ++y) for (int x = 0; x < slices; ++x) {
        const unsigned a = y * (slices + 1) + x;
        const unsigned b = a + slices + 1;
        indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
    indexCount_ = GLsizei(indices.size());
    vertexArray_.create();
    vertexArray_.bind();
    vertexBuffer_.create();
    vertexBuffer_.bind();
    vertexBuffer_.allocate(vertices.data(), int(vertices.size() * sizeof(float)));
    indexBuffer_.create();
    indexBuffer_.bind();
    indexBuffer_.allocate(indices.data(), int(indices.size() * sizeof(unsigned int)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    vertexArray_.release();
    vertexBuffer_.release();
    indexBuffer_.release();
    glGenTextures(1, &texture_);
    glGenTextures(1, &maskTexture_);
    fullTextureDirty_ = true;
    fullMaskDirty_ = true;
}
void SphereView::resizeGL(int, int) {}
void SphereView::uploadTexture() {
    if ((!fullTextureDirty_ && partialTextureDirty_.isEmpty()) || !texture_ || composite_.isNull()) return;
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (fullTextureDirty_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, composite_.width(), composite_.height(),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, composite_.constBits());
    } else {
        // Upload edited rows/rectangles only, instead of the entire 8K/16K image.
        glPixelStorei(GL_UNPACK_ROW_LENGTH, composite_.width());
        for (const QRect &rect : partialTextureDirty_) {
            glPixelStorei(GL_UNPACK_SKIP_ROWS, rect.y());
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, rect.x());
            glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x(), rect.y(), rect.width(), rect.height(),
                            GL_RGBA, GL_UNSIGNED_BYTE, composite_.constBits());
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    fullTextureDirty_ = false;
    partialTextureDirty_ = {};
}
void SphereView::uploadMaskTexture() {
    if (aiMask_.isNull() || !maskTexture_ ||
        (!fullMaskDirty_ && aiMaskDirty_.isEmpty())) return;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, maskTexture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, aiMask_.bytesPerLine());
    if (fullMaskDirty_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, aiMask_.width(), aiMask_.height(),
                     0, GL_RED, GL_UNSIGNED_BYTE, aiMask_.constBits());
    } else {
        for (const QRect &rect : aiMaskDirty_) {
            glPixelStorei(GL_UNPACK_SKIP_ROWS, rect.y());
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, rect.x());
            glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x(), rect.y(), rect.width(), rect.height(),
                            GL_RED, GL_UNSIGNED_BYTE, aiMask_.constBits());
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    fullMaskDirty_ = false;
    aiMaskDirty_ = {};
    glActiveTexture(GL_TEXTURE0);
}
void SphereView::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!shader_ || composite_.isNull()) return;
    uploadTexture();
    uploadMaskTexture();
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    QMatrix4x4 projection;
    projection.perspective(fov_, float(width()) / std::max(1, height()), 0.01f, 10.0f);
    QMatrix4x4 view;
    view.rotate(-pitch_, 1, 0, 0);
    view.rotate(-yaw_, 0, 1, 0);
    shader_->bind();
    shader_->setUniformValue("uMVP", projection * view);
    shader_->setUniformValue("uPanorama", 0);
    shader_->setUniformValue("uMask", 1);
    shader_->setUniformValue("uShowMask", hasAiSelection_ && !aiMask_.isNull());
    shader_->setUniformValue("uShowCursor", cursorInside_ && mode_ != Mode::Navigate &&
                                           !dragging_ && !composite_.isNull());
    shader_->setUniformValue("uCursorDirection", cursorDirection_);
    shader_->setUniformValue("uCursorRadius", float(brushRadius_) * rad);
    shader_->setUniformValue("uCursorPixelAngle", std::max(0.0002f,
        fov_ * rad / float(std::max(1, height()))));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, maskTexture_);
    glActiveTexture(GL_TEXTURE0);
    vertexArray_.bind();
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    vertexArray_.release();
    shader_->release();
    if (mode_ == Mode::RoundObject && roundCenter_) {
        QPainter painter(this);
        painter.setPen(QPen(Qt::cyan, 2));
        const auto mark = [&](const QVector3D &d) {
            const QVector4D p = projection * view * QVector4D(d, 1);
            if (p.w() > 0) painter.drawEllipse(QPointF((p.x()/p.w()+1)*width()/2, (1-p.y()/p.w())*height()/2), 4, 4);
        };
        mark(*roundCenter_); for (const auto &d : roundContour_) mark(d);
    }
}
QVector3D SphereView::directionFromScreen(const QPointF &pos) const {
    // Use the same projection and camera matrices for painting and rendering.
    QMatrix4x4 projection;
    projection.perspective(fov_, float(width()) / std::max(1, height()), 0.01f, 10.0f);
    QMatrix4x4 view;
    view.rotate(-pitch_, 1, 0, 0);
    view.rotate(-yaw_, 0, 1, 0);
    bool invertible = false;
    const QMatrix4x4 inverse = (projection * view).inverted(&invertible);
    if (!invertible) return QVector3D(0, 0, -1);
    const float nx = 2.0f * float(pos.x()) / std::max(1, width()) - 1.0f;
    const float ny = 1.0f - 2.0f * float(pos.y()) / std::max(1, height());
    const QVector4D nearPoint = inverse * QVector4D(nx, ny, -1.0f, 1.0f);
    const QVector4D farPoint = inverse * QVector4D(nx, ny, 1.0f, 1.0f);
    return (farPoint.toVector3DAffine() - nearPoint.toVector3DAffine()).normalized();
}
void SphereView::paintAt(const QVector3D &direction) {
    if (editLayer_.isNull()) return;
    if (mode_ == Mode::AiSharpen || mode_ == Mode::AiInpaint) {
        if (aiMask_.isNull()) {
            aiMask_ = QImage(baseImage_.size(), QImage::Format_Grayscale8);
            aiMask_.fill(0);
            fullMaskDirty_ = true;
        }
        const QRegion region = SphericalAI::paintMask(aiMask_, direction, brushRadius_);
        if (!region.isEmpty()) {
            aiMaskDirty_ += region;
            hasAiSelection_ = true;
            update();
        }
        return;
    }
    if (mode_ == Mode::Smudge || mode_ == Mode::Push ||
        mode_ == Mode::Bloat || mode_ == Mode::Pinch) {
        SphericalLiquify::Tool tool = SphericalLiquify::Tool::Smudge;
        if (mode_ == Mode::Push) tool = SphericalLiquify::Tool::Push;
        if (mode_ == Mode::Bloat) tool = SphericalLiquify::Tool::Bloat;
        if (mode_ == Mode::Pinch) tool = SphericalLiquify::Tool::Pinch;
        const QVector3D previous = lastStroke_.value_or(direction);
        const QRegion region = SphericalLiquify::apply(editLayer_, composite_,
            composite_, direction, previous, brushRadius_, toolStrength_, tool);
        if (region.isEmpty()) return;
        partialTextureDirty_ += region;
        if (!strokeHasChanged_) {
            strokeHasChanged_ = true;
            revision_ = nextRevision_++;
        }
        setModified(revision_ != savedRevision_);
        update();
        return;
    }
    SphericalRaster::Tool tool = SphericalRaster::Tool::Brush;
    if (mode_ == Mode::Eraser) tool = SphericalRaster::Tool::Eraser;
    if (mode_ == Mode::CloneStamp) tool = SphericalRaster::Tool::CloneStamp;
    const QImage *sampleImage = tool == SphericalRaster::Tool::CloneStamp ? &cloneSnapshot_ : nullptr;
    const QRegion region = SphericalRaster::applyDab(editLayer_, composite_, baseImage_, sampleImage,
        direction, brushRadius_, tool, brushColor_, cloneRotation_);
    if (region.isEmpty()) return;
    partialTextureDirty_ += region;
    if (!strokeHasChanged_) {
        strokeHasChanged_ = true;
        revision_ = nextRevision_++;
    }
    setModified(revision_ != savedRevision_);
    update();
}
void SphereView::strokeBetween(const QVector3D &a, const QVector3D &b) {
    const float angle = std::acos(clampf(QVector3D::dotProduct(a, b), -1, 1));
    const float spacing = std::max(0.001f, float(brushRadius_) * rad * 0.4f);
    const int steps = std::clamp(int(std::ceil(angle / spacing)), 1, 4096);
    for (int i = 1; i <= steps; ++i) {
        const float t = float(i) / steps;
        // Normalized interpolation is accurate for short mouse-move segments.
        if (mode_ == Mode::Smudge || mode_ == Mode::Push) {
            const float prevT = float(i - 1) / steps;
            lastStroke_ = ((1 - prevT) * a + prevT * b).normalized();
        }
        paintAt(((1 - t) * a + t * b).normalized());
    }
}
void SphereView::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) return;
    cursorInside_ = true;
    cursorDirection_ = directionFromScreen(event->position());
    lastMouse_ = event->pos();
    if (mode_ == Mode::RoundObject && event->button() == Qt::LeftButton && hasImage()) {
        if (!roundCenter_) roundCenter_ = cursorDirection_;
        else roundContour_.append(cursorDirection_);
        update(); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && mode_ == Mode::CloneStamp &&
        (event->modifiers() & Qt::ControlModifier) && hasImage()) {
        cloneSource_ = directionFromScreen(event->position());
        emit statusMessage(tr("Источник штампа выбран. Проведите левой кнопкой, чтобы скопировать на сферу."));
    } else if (mode_ != Mode::Navigate && event->button() == Qt::LeftButton && hasImage()) {
        if (mode_ == Mode::CloneStamp && !cloneSource_) {
            emit statusMessage(tr("Сначала выберите источник: Ctrl + щелчок левой кнопкой."));
            event->accept();
            return;
        }
        strokeHasChanged_ = false;
        if (mode_ != Mode::AiSharpen && mode_ != Mode::AiInpaint)
            pushUndo();
        painting_ = true;
        lastStroke_ = directionFromScreen(event->position());
        if (mode_ == Mode::CloneStamp) {
            cloneSnapshot_ = composite_;
            cloneRotation_ = QQuaternion::rotationTo(*lastStroke_, *cloneSource_);
        }
        if (mode_ != Mode::Smudge && mode_ != Mode::Push)
            paintAt(*lastStroke_);
    } else dragging_ = true;
    event->accept();
}
void SphereView::mouseMoveEvent(QMouseEvent *event) {
    cursorInside_ = rect().contains(event->pos());
    if (painting_ && (event->buttons() & Qt::LeftButton)) {
        const auto next = directionFromScreen(event->position());
        if (lastStroke_) strokeBetween(*lastStroke_, next);
        else paintAt(next);
        lastStroke_ = next;
    } else if (dragging_ && (event->buttons() & (Qt::LeftButton | Qt::RightButton))) {
        const QPoint delta = event->pos() - lastMouse_;
        yaw_ += delta.x() * 0.22f;
        pitch_ = clampf(pitch_ + delta.y() * 0.22f, -89.9f, 89.9f);
        update();
    }
    // Compute after navigation, so the ring remains centered beneath the pointer.
    cursorDirection_ = directionFromScreen(event->position());
    lastMouse_ = event->pos();
    update(); // Redraw the cursor even when no mouse button is pressed.
    event->accept();
}
void SphereView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        painting_ = false;
        lastStroke_.reset();
        cloneSnapshot_ = {};
    }
    dragging_ = false;
    update();
    event->accept();
}
void SphereView::wheelEvent(QWheelEvent *event) {
    fov_ = clampf(fov_ - event->angleDelta().y() / 120.0f * 4.0f, 20.0f, 110.0f);
    cursorDirection_ = directionFromScreen(event->position());
    update();
    event->accept();
}
void SphereView::enterEvent(QEnterEvent *event) {
    cursorInside_ = true;
    cursorDirection_ = directionFromScreen(event->position());
    update();
    QOpenGLWidget::enterEvent(event);
}
void SphereView::leaveEvent(QEvent *event) {
    cursorInside_ = false;
    update();
    QOpenGLWidget::leaveEvent(event);
}

void SphereView::roundObject() {
    if (mode_ != Mode::RoundObject || !roundCenter_ || roundContour_.size() < 4 || !hasImage()) {
        emit statusMessage(tr("Укажите центр и минимум четыре точки по контуру объекта.")); return;
    }
    const QVector3D c = *roundCenter_;
    const QVector3D axis = std::abs(c.y()) < 0.9f ? QVector3D(0,1,0) : QVector3D(1,0,0);
    const QVector3D u = QVector3D::crossProduct(axis,c).normalized();
    const QVector3D v = QVector3D::crossProduct(c,u).normalized();
    std::vector<std::pair<double,double>> samples;
    double target = 0, outer = 0;
    for (const auto &d : roundContour_) {
        double a = std::atan2(QVector3D::dotProduct(d,v), QVector3D::dotProduct(d,u));
        double r = std::acos(clampf(QVector3D::dotProduct(c,d),-1,1));
        if (r < 0.001 || r > PI/3) continue;
        samples.emplace_back(a,r); target += r; outer = std::max(outer,r);
    }
    if (samples.size() < 4) return;
    std::sort(samples.begin(),samples.end());
    target /= samples.size();
    for (size_t i=0;i<samples.size();++i) {
        double gap = (i+1<samples.size()?samples[i+1].first:samples.front().first+2*PI)-samples[i].first;
        if (gap > PI/2+0.01) { emit statusMessage(tr("Добавьте точки по всему контуру без больших промежутков.")); return; }
    }
    outer = std::min(double(PI)*0.49, outer*1.35);
    samples.insert(samples.begin(), {samples.back().first-2*PI,samples.back().second});
    samples.push_back({samples[1].first+2*PI,samples[1].second});
    const QImage source = composite_;
    pushUndo();
    const int w=source.width(), h=source.height();
    QRegion dirty;
    for (int y=0;y<h;++y) {
        double lat=PI*(0.5-(y+0.5)/h);
        if (std::abs(lat-std::asin(clampf(c.y(),-1,1)))>outer) continue;
        int start=-1;
        for (int x=0;x<w;++x) {
            const auto d=SphericalRaster::pixelDirection(x,y,w,h);
            double r=std::acos(clampf(QVector3D::dotProduct(c,d),-1,1));
            if (r>=outer || r<1e-7) {
                if(start>=0){dirty+=QRect(start,y,x-start,1);start=-1;} continue;
            }
            double a=std::atan2(QVector3D::dotProduct(d,v),QVector3D::dotProduct(d,u));
            auto hi=std::upper_bound(samples.begin(),samples.end(),a,[](double x,const auto &p){return x<p.first;});
            if(hi==samples.begin()||hi==samples.end()) continue;
            auto lo=hi-1;
            double t=(a-lo->first)/std::max(1e-9,hi->first-lo->first);
            double old=lo->second+(hi->second-lo->second)*t;
            double desired=old+(target-old)*toolStrength_;
            double sr=r<=desired?r*old/desired:old+(r-desired)*(outer-old)/(outer-desired);
            const auto tangent=(d-c*float(std::cos(r))).normalized();
            auto p=SphericalRaster::sampleDirection(source,c*float(std::cos(sr))+tangent*float(std::sin(sr)));
            QColor color=QColor::fromRgb(p);
            editLayer_.setPixelColor(x,y,color); composite_.setPixelColor(x,y,color);
            if(start<0)start=x;
        }
        if(start>=0)dirty+=QRect(start,y,w-start,1);
    }
    partialTextureDirty_+=dirty; revision_=nextRevision_++; setModified(revision_!=savedRevision_);
    roundCenter_.reset();roundContour_.clear();update();
    emit statusMessage(tr("Округление применено. Ctrl+Z — отменить."));
}
