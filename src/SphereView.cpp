#include "SphereView.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QOpenGLContext>
#include <QMatrix4x4>
#include <QVector2D>
#include <QFileInfo>
#include <QtMath>
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
    setCursor(mode == Mode::Brush ? Qt::CrossCursor : Qt::OpenHandCursor);
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
    image_ = loaded.convertToFormat(QImage::Format_RGBA8888);
    undoStack_.clear();
    redoStack_.clear();
    yaw_ = pitch_ = 0;
    textureDirty_ = true;
    setModified(false);
    update();
    emit imageChanged();
    return true;
}
bool SphereView::saveImage(const QString &path) const {
    if (image_.isNull()) return false;
    // Avoid mutating modified_ from a const method; caller handles filename state.
    return image_.save(path);
}
void SphereView::pushUndo() {
    if (image_.isNull()) return;
    if (undoStack_.size() >= historyLimit_) undoStack_.removeFirst();
    undoStack_.push_back(image_);
    redoStack_.clear();
}
void SphereView::undo() {
    if (undoStack_.empty() || image_.isNull()) return;
    redoStack_.push_back(image_);
    image_ = undoStack_.takeLast();
    textureDirty_ = true;
    setModified(true);
    update();
}
void SphereView::redo() {
    if (redoStack_.empty() || image_.isNull()) return;
    undoStack_.push_back(image_);
    image_ = redoStack_.takeLast();
    textureDirty_ = true;
    setModified(true);
    update();
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
            vertices.insert(vertices.end(), {d.x(), d.y(), d.z(), u, 1.0f - v});
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
    textureDirty_ = true;
}
void SphereView::resizeGL(int, int) {}
void SphereView::uploadTexture() {
    if (!textureDirty_ || !texture_ || image_.isNull()) return;
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image_.width(), image_.height(),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, image_.constBits());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    textureDirty_ = false;
}
void SphereView::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!shader_ || image_.isNull()) return;
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
    // Unproject through the exact same camera matrices used in paintGL().
    // This keeps the brush aligned after zoom, yaw/pitch and window resizing.
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
    const QVector3D nearWorld = nearPoint.toVector3DAffine();
    const QVector3D farWorld = farPoint.toVector3DAffine();
    return (farWorld - nearWorld).normalized();
}
void SphereView::paintAt(const QVector3D &direction) {
    if (image_.isNull()) return;
    const float longitude = std::atan2(direction.x(), -direction.z());
    const float latitude = std::asin(clampf(direction.y(), -1, 1));
    const int w = image_.width(), h = image_.height();
    const float radius = float(brushRadius_) * rad;
    const int cy = std::clamp(int((0.5f - latitude / PI) * h), 0, h - 1);
    const int ry = int(std::ceil(radius * h / PI)) + 1;
    const int y0 = std::max(0, cy - ry), y1 = std::min(h - 1, cy + ry);
    const auto rgba = brushColor_.rgba();
    const float ca = brushColor_.alphaF();
    for (int y = y0; y <= y1; ++y) {
        const float phi = PI * (0.5f - (y + 0.5f) / h);
        const float sinPhi = std::sin(phi), cosPhi = std::cos(phi);
        const float sinLat = std::sin(latitude), cosLat = std::cos(latitude);
        const float denom = cosPhi * cosLat;
        const float threshold = (std::cos(radius) - sinPhi * sinLat) / denom;
        if (threshold > 1.0f) continue;
        const float span = (denom < 1e-7f || threshold <= -1.0f) ? PI : std::acos(clampf(threshold, -1, 1));
        const int cx = int((longitude / (2 * PI) + 0.5f) * w);
        const int rx = int(std::ceil(span / (2 * PI) * w)) + 1;
        const int first = rx >= w / 2 ? 0 : cx - rx;
        const int last = rx >= w / 2 ? w - 1 : cx + rx;
        uchar *row = image_.scanLine(y);
        for (int xi = first; xi <= last; ++xi) {
            const int x = (xi % w + w) % w;
            const float theta = (float(x + 0.5f) / w - 0.5f) * 2 * PI;
            const float dot = sinPhi * sinLat + cosPhi * cosLat * std::cos(theta - longitude);
            if (dot < std::cos(radius)) continue;
            uchar *px = row + x * 4;
            px[0] = uchar(std::lround(px[0] * (1 - ca) + qRed(rgba) * ca));
            px[1] = uchar(std::lround(px[1] * (1 - ca) + qGreen(rgba) * ca));
            px[2] = uchar(std::lround(px[2] * (1 - ca) + qBlue(rgba) * ca));
            px[3] = 255;
        }
    }
    textureDirty_ = true;
    setModified(true);
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
    if (mode_ == Mode::Brush && event->button() == Qt::LeftButton && hasImage()) {
        pushUndo();
        painting_ = true;
        lastStroke_ = directionFromScreen(event->position());
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
    }
    dragging_ = false;
    event->accept();
}
void SphereView::wheelEvent(QWheelEvent *event) {
    fov_ = clampf(fov_ - event->angleDelta().y() / 120.0f * 4.0f, 20.0f, 110.0f);
    update();
    event->accept();
}
