#include "SphereView.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QOpenGLContext>
#include <QMatrix4x4>
#include <cmath>
#include <algorithm>
#include <vector>

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
        vertexArray_.destroy();
        vertexBuffer_.destroy();
        indexBuffer_.destroy();
        doneCurrent();
    }
}
void SphereView::setMode(Mode mode) {
    mode_ = mode;
    setCursor(mode == Mode::Navigate ? Qt::OpenHandCursor : Qt::CrossCursor);
    if (mode == Mode::CloneStamp)
        emit statusMessage(tr("Clone Stamp: Ctrl + left-click to choose source, then paint."));
}
void SphereView::setModified(bool modified) {
    if (modified_ == modified) return;
    modified_ = modified;
    emit modifiedChanged(modified);
}
bool SphereView::openImage(const QString &path, QString *error) {
    QImage loaded(path);
    if (loaded.isNull()) {
        if (error) *error = tr("Image could not be decoded: %1").arg(path);
        return false;
    }
    if (loaded.width() != 2 * loaded.height()) {
        if (error) *error = tr("Expected a 2:1 equirectangular panorama, got %1 × %2.").arg(loaded.width()).arg(loaded.height());
        return false;
    }
    baseImage_ = loaded.convertToFormat(QImage::Format_RGBA8888);
    editLayer_ = QImage(baseImage_.size(), QImage::Format_RGBA8888);
    editLayer_.fill(Qt::transparent);
    composite_ = baseImage_;
    cloneSnapshot_ = {};
    cloneSource_.reset();
    undoStack_.clear();
    redoStack_.clear();
    revision_ = savedRevision_ = 0;
    nextRevision_ = 1;
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
void SphereView::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.075f, 0.085f, 0.11f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    shader_ = std::make_unique<QOpenGLShaderProgram>();
    const char *vs = R"(#version 330 core
layout(location=0) in vec3 inPosition;
layout(location=1) in vec2 inUV;
out vec2 vUV;
uniform mat4 uMVP;
void main() { vUV = inUV; gl_Position = uMVP * vec4(inPosition, 1.0); }
)";
    const char *fs = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uPanorama;
void main() { fragColor = texture(uPanorama, vUV); }
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
    fullTextureDirty_ = true;
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
void SphereView::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!shader_ || composite_.isNull()) return;
    uploadTexture();
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
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    vertexArray_.bind();
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    vertexArray_.release();
    shader_->release();
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
        paintAt(((1 - t) * a + t * b).normalized());
    }
}
void SphereView::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) return;
    lastMouse_ = event->pos();
    if (event->button() == Qt::LeftButton && mode_ == Mode::CloneStamp &&
        (event->modifiers() & Qt::ControlModifier) && hasImage()) {
        cloneSource_ = directionFromScreen(event->position());
        emit statusMessage(tr("Clone source set. Left-drag to copy onto the sphere."));
    } else if (mode_ != Mode::Navigate && event->button() == Qt::LeftButton && hasImage()) {
        if (mode_ == Mode::CloneStamp && !cloneSource_) {
            emit statusMessage(tr("First choose clone source with Ctrl + left-click."));
            event->accept();
            return;
        }
        strokeHasChanged_ = false;
        pushUndo();
        painting_ = true;
        lastStroke_ = directionFromScreen(event->position());
        if (mode_ == Mode::CloneStamp) {
            cloneSnapshot_ = composite_;
            cloneRotation_ = QQuaternion::rotationTo(*lastStroke_, *cloneSource_);
        }
        paintAt(*lastStroke_);
    } else dragging_ = true;
    event->accept();
}
void SphereView::mouseMoveEvent(QMouseEvent *event) {
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
    lastMouse_ = event->pos();
    event->accept();
}
void SphereView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        painting_ = false;
        lastStroke_.reset();
        cloneSnapshot_ = {};
    }
    dragging_ = false;
    event->accept();
}
void SphereView::wheelEvent(QWheelEvent *event) {
    fov_ = clampf(fov_ - event->angleDelta().y() / 120.0f * 4.0f, 20.0f, 110.0f);
    update();
    event->accept();
}
