#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSlider>
#include <QCheckBox>
#include <QInputDialog>
#include <QStringList>
#include <QPushButton>
#include <QMessageBox>
#include "AiEngine.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
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
    resetMesh();
    mode_ = mode;
    if (mode == Mode::Straighten) emit statusMessage(tr("Отметьте минимум три точки линии по порядку, затем примените."));
    if (mode == Mode::MeshWarp) emit statusMessage(tr("Сетка: выберите центр, перетаскивайте внутренние узлы, затем примените."));
    roundCenter_.reset(); roundContour_.clear(); linePoints_.clear();
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
    if (meshCenter_) meshExtent_ = std::tan(float(brushRadius_) * rad);
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
    roundCenter_.reset(); roundContour_.clear(); linePoints_.clear();
    resetMesh();
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
    if (meshCenter_) applyMesh();
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
    if (!linePoints_.isEmpty()) { linePoints_.removeLast(); update(); return; }
    if (meshCenter_) { resetMesh(); return; }
    if (undoStack_.empty() || editLayer_.isNull()) return;
    redoStack_.push_back({editLayer_, revision_});
    const auto prior = undoStack_.takeLast();
    editLayer_ = prior.layer;
    revision_ = prior.revision;
    refreshComposite();
}
void SphereView::redo() {
    resetMesh();
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
    ++selectionRequest_;
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
uniform bool uColorPreview;
uniform bool uLevelPreview;
uniform vec4 uLevelRotation;
uniform float uColorValues[6];
uniform bool uMesh;
uniform vec3 uMeshC, uMeshU, uMeshV;
uniform float uMeshExtent, uMeshStrength;
uniform vec2 uMeshOffsets[16];
vec3 meshDirection(vec3 d) {
    float z = dot(d,uMeshC);
    if (!uMesh || z <= 0.0) return d;
    vec2 p = vec2(dot(d,uMeshU),dot(d,uMeshV))/z/uMeshExtent;
    if (any(greaterThanEqual(abs(p),vec2(1.0)))) return d;
    vec2 g = (p+1.0)*1.5;
    ivec2 i = ivec2(floor(g)); vec2 f = fract(g);
    int k = i.y*4+i.x;
    vec2 off = mix(mix(uMeshOffsets[k],uMeshOffsets[k+1],f.x),mix(uMeshOffsets[k+4],uMeshOffsets[k+5],f.x),f.y);
    p += off*uMeshStrength;
    return normalize(uMeshC+uMeshExtent*(p.x*uMeshU+p.y*uMeshV));
}
uniform bool uShowCursor;
uniform vec3 uCursorDirection;
uniform float uCursorRadius;
uniform float uCursorPixelAngle;
const float PI = 3.14159265358979323846;
void main() {
    vec3 d = normalize(vDirection);
    vec3 sampleD = meshDirection(d);
    if(uLevelPreview){vec3 q=uLevelRotation.xyz;sampleD=normalize(sampleD+2.0*cross(q,cross(q,sampleD)+uLevelRotation.w*sampleD));}
    float u = 0.5 + atan(sampleD.x, -sampleD.z) / (2.0 * PI);
    // OpenGL treats the first row of QImage as the bottom texture row (v=0).
    float v = 0.5 - asin(clamp(sampleD.y, -1.0, 1.0)) / PI;
    fragColor = texture(uPanorama, vec2(u, v));
    if (uColorPreview) {
        vec3 rgb=fragColor.rgb;
        float lum=dot(rgb,vec3(0.2126,0.7152,0.0722));
        float tone=uColorValues[0]*0.25+uColorValues[4]*0.35*(1.0-lum)*(1.0-lum)+uColorValues[5]*0.35*lum*lum;
        vec3 z=(rgb-lum)*(1.0+uColorValues[2])+lum;
        z=(z-0.5)*(1.0+uColorValues[1])+0.5+tone;
        z+=uColorValues[3]*0.12*vec3(1,0,-1);
        fragColor.rgb=mix(rgb,clamp(z,0.0,1.0),texture(uMask,vec2(u,v)).r);
    }
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
    shader_->setUniformValue("uMesh", mode_ == Mode::MeshWarp && meshCenter_.has_value());
    if (meshCenter_) {
        shader_->setUniformValue("uMeshC", *meshCenter_);
        shader_->setUniformValue("uMeshU", meshU_);
        shader_->setUniformValue("uMeshV", meshV_);
        shader_->setUniformValue("uMeshExtent", meshExtent_);
        shader_->setUniformValue("uMeshStrength", float(toolStrength_));
        shader_->setUniformValueArray("uMeshOffsets", meshOffsets_.constData(), 16);
    }
    shader_->setUniformValue("uMask", 1);
    shader_->setUniformValue("uColorPreview",colorPreview_);
    shader_->setUniformValue("uLevelPreview",levelPreview_);
    shader_->setUniformValue("uLevelRotation",QVector4D(levelRotation_.x(),levelRotation_.y(),levelRotation_.z(),levelRotation_.scalar()));
    shader_->setUniformValueArray("uColorValues",colorValues_,6,1);
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
    if(levelPreview_){QPainter p(this);p.setPen(QPen(Qt::yellow,1,Qt::DashLine));p.drawLine(0,height()/2,width(),height()/2);}
    if (diagnosticDocument_==documentId_ && diagnosticRevision_==revision_ && !diagnosticMarks_.isEmpty()) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        for(const auto &m:diagnosticMarks_){
            auto p=projection*view*QVector4D(m.direction,1);
            if(p.w()<=0.001f)continue;
            QPointF pos((p.x()/p.w()+1)*width()/2,(1-p.y()/p.w())*height()/2);
            if(!rect().adjusted(-20,-20,20,20).contains(pos.toPoint()))continue;
            painter.setPen(QPen(m.color,2));
            QColor fill=m.color;fill.setAlpha(65);painter.setBrush(fill);
            painter.drawEllipse(pos,13,13);
            painter.drawText(pos+QPointF(17,-8),m.label);
        }
    }
    if (mode_ == Mode::MeshWarp && meshCenter_) {
        QPainter painter(this); painter.setPen(QPen(Qt::cyan, 1));
        QPointF pts[16]; bool visible[16];
        for(int i=0;i<16;++i) {
            QVector2D p(-1.f+2.f*(i%4)/3.f,-1.f+2.f*(i/4)/3.f);
            p -= meshOffsets_[i]*float(toolStrength_);
            QVector4D q=projection*view*QVector4D((*meshCenter_+meshExtent_*(p.x()*meshU_+p.y()*meshV_)).normalized(),1);
            visible[i]=q.w()>0; if(visible[i]) pts[i]=QPointF((q.x()/q.w()+1)*width()/2,(1-q.y()/q.w())*height()/2);
        }
        for(int i=0;i<16;++i) if(visible[i]) {
            if(i%4<3 && visible[i+1]) painter.drawLine(pts[i],pts[i+1]);
            if(i/4<3 && visible[i+4]) painter.drawLine(pts[i],pts[i+4]);
            if(i%4>0 && i%4<3 && i/4>0 && i/4<3) painter.drawEllipse(pts[i],5,5);
        }
    }
    if (mode_ == Mode::Straighten && !linePoints_.isEmpty()) {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        auto project=[&](const QVector3D &d)->std::optional<QPointF>{
            auto p=projection*view*QVector4D(d,1);
            if(p.w()<=0.001f) return {};
            return QPointF((p.x()/p.w()+1)*width()/2,(1-p.y()/p.w())*height()/2);
        };
        painter.setPen(QPen(Qt::yellow,2));
        std::optional<QPointF> prev;
        for(const auto &d:linePoints_) {
            auto p=project(d); if(p){painter.drawEllipse(*p,4,4);if(prev)painter.drawLine(*prev,*p);} prev=p;
        }
        auto a=project(linePoints_.front()), b=project(linePoints_.back());
        painter.setPen(QPen(Qt::cyan,2,Qt::DashLine)); if(a&&b)painter.drawLine(*a,*b);
    }
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
    if(mode_==Mode::SmartSelect && event->button()==Qt::LeftButton && hasImage()){
        smartSelect(cursorDirection_,event->modifiers() & Qt::AltModifier ? -1 : event->modifiers() & Qt::ShiftModifier ? 1 : 0);
        event->accept();return;
    }
    if (mode_ == Mode::Straighten && event->button()==Qt::LeftButton && hasImage()) {
        if (linePoints_.size()<128) linePoints_.append(cursorDirection_);
        update(); event->accept(); return;
    }
    if (mode_ == Mode::MeshWarp && event->button()==Qt::LeftButton && hasImage()) {
        if (!meshCenter_) {
            meshCenter_=cursorDirection_;
            meshU_=QVector3D::crossProduct(std::abs(cursorDirection_.y())<0.9f?QVector3D(0,1,0):QVector3D(1,0,0),cursorDirection_).normalized();
            meshV_=QVector3D::crossProduct(cursorDirection_,meshU_).normalized();
            meshExtent_=std::tan(float(brushRadius_)*rad);
            meshOffsets_.fill(QVector2D(),16);
        } else {
            float z=QVector3D::dotProduct(cursorDirection_,*meshCenter_);
            if(z>0) {
                QVector2D p(QVector3D::dotProduct(cursorDirection_,meshU_)/z/meshExtent_,QVector3D::dotProduct(cursorDirection_,meshV_)/z/meshExtent_);
                float best=0.25f;
                for(int i: {5,6,9,10}) {
                    QVector2D node(-1.f+2.f*(i%4)/3.f,-1.f+2.f*(i/4)/3.f);
                    float dist=(p-node+meshOffsets_[i]*float(toolStrength_)).length();
                    if(dist<best){best=dist;meshDrag_=i;}
                }
            }
        }
        update(); event->accept(); return;
    }
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
    if (mode_==Mode::MeshWarp && meshCenter_ && meshDrag_>=0 && (event->buttons() & Qt::LeftButton)) {
        auto d=directionFromScreen(event->position()); float z=QVector3D::dotProduct(d,*meshCenter_);
        if(z>0) {
            QVector2D p(QVector3D::dotProduct(d,meshU_)/z/meshExtent_,QVector3D::dotProduct(d,meshV_)/z/meshExtent_);
            QVector2D node(-1.f+2.f*(meshDrag_%4)/3.f,-1.f+2.f*(meshDrag_/4)/3.f);
            auto off=(node-p)/float(std::max(0.01,toolStrength_));
            meshOffsets_[meshDrag_]=QVector2D(clampf(off.x(),-0.15f,0.15f),clampf(off.y(),-0.15f,0.15f));
        }
    } else if (painting_ && (event->buttons() & Qt::LeftButton)) {
        const auto next = directionFromScreen(event->position());
        if (lastStroke_) strokeBetween(*lastStroke_, next);
        else paintAt(next);
        lastStroke_ = next;
    } else if (dragging_ && (event->buttons() & (Qt::LeftButton | Qt::RightButton))) {
        const QPoint delta = event->pos() - lastMouse_;
        yaw_ += delta.x() * 0.22f;
        if (!poleView_) pitch_ = clampf(pitch_ + delta.y() * 0.22f, -89.9f, 89.9f);
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
        meshDrag_ = -1;
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

void SphereView::resetMesh() {
    meshCenter_.reset(); meshOffsets_.clear(); meshDrag_=-1; update();
}
void SphereView::applyMesh() {
    if (!meshCenter_ || meshOffsets_.size()!=16 || mode_!=Mode::MeshWarp) return;
    bool changed=false; for(const auto &o:meshOffsets_) if(!o.isNull()) changed=true;
    if(!changed){resetMesh();return;}
    const QImage source=composite_;
    pushUndo();
    QRegion dirty;
    for(int y=0;y<source.height();++y) {
        int start=-1;
        for(int x=0;x<source.width();++x) {
            auto d=SphericalRaster::pixelDirection(x,y,source.width(),source.height());
            float z=QVector3D::dotProduct(d,*meshCenter_);
            float px=QVector3D::dotProduct(d,meshU_)/std::max(z,1e-8f)/meshExtent_;
            float py=QVector3D::dotProduct(d,meshV_)/std::max(z,1e-8f)/meshExtent_;
            if(z<=0 || std::abs(px)>=1 || std::abs(py)>=1) {
                if(start>=0){dirty+=QRect(start,y,x-start,1);start=-1;} continue;
            }
            float gx=(px+1)*1.5f,gy=(py+1)*1.5f;
            int ix=std::min(2,int(gx)),iy=std::min(2,int(gy)),k=iy*4+ix;
            float fx=gx-ix,fy=gy-iy;
            auto off=((meshOffsets_[k]*(1-fx)+meshOffsets_[k+1]*fx)*(1-fy)+(meshOffsets_[k+4]*(1-fx)+meshOffsets_[k+5]*fx)*fy)*float(toolStrength_);
            auto sample=(*meshCenter_+meshExtent_*((px+off.x())*meshU_+(py+off.y())*meshV_)).normalized();
            auto color=QColor::fromRgb(SphericalRaster::sampleDirection(source,sample));
            editLayer_.setPixelColor(x,y,color); composite_.setPixelColor(x,y,color);
            if(start<0)start=x;
        }
        if(start>=0)dirty+=QRect(start,y,source.width()-start,1);
    }
    partialTextureDirty_+=dirty; revision_=nextRevision_++;setModified(revision_!=savedRevision_);resetMesh();
    emit statusMessage(tr("Деформация сетки применена. Ctrl+Z — отменить."));
}

void SphereView::resetLine() { linePoints_.clear(); update(); }
void SphereView::straightenLine() {
    if(mode_!=Mode::Straighten || linePoints_.size()<3 || !hasImage()) {
        emit statusMessage(tr("Укажите минимум три точки линии.")); return;
    }
    const auto a=linePoints_.front(), b=linePoints_.back();
    const auto cross=QVector3D::crossProduct(a,b);
    const float length=std::acos(clampf(QVector3D::dotProduct(a,b),-1,1));
    if(cross.length()<0.001f || length>2.6f) { emit statusMessage(tr("Выберите более короткий участок линии.")); return; }
    const auto n=cross.normalized(), t=QVector3D::crossProduct(n,a).normalized();
    std::vector<std::pair<float,float>> samples;
    float maxOffset=0;
    for(const auto &d:linePoints_) {
        float x=std::atan2(QVector3D::dotProduct(d,t),QVector3D::dotProduct(d,a));
        float y=std::asin(clampf(QVector3D::dotProduct(d,n),-1,1));
        if(!samples.empty() && x<=samples.back().first+0.00001f) {
            emit statusMessage(tr("Точки должны идти последовательно от начала к концу.")); return;
        }
        samples.emplace_back(x,y); maxOffset=std::max(maxOffset,std::abs(y));
    }
    samples.front()={0,0}; samples.back()={length,0};
    if(maxOffset<0.00001f || toolStrength_<=0) { resetLine(); return; }
    const float radius=std::max(float(brushRadius_)*rad,2.f*maxOffset+0.001f);
    if(radius>1.2f) {emit statusMessage(tr("Слишком сильное искривление. Разделите линию на участки."));return;}
    const QImage source=composite_; pushUndo();
    QRegion dirty;
    for(int y=0;y<source.height();++y) {
        int start=-1;
        for(int x=0;x<source.width();++x) {
            auto d=SphericalRaster::pixelDirection(x,y,source.width(),source.height());
            float along=std::atan2(QVector3D::dotProduct(d,t),QVector3D::dotProduct(d,a));
            float across=std::asin(clampf(QVector3D::dotProduct(d,n),-1,1));
            if(along<=0 || along>=length || std::abs(across)>=radius) {
                if(start>=0){dirty+=QRect(start,y,x-start,1);start=-1;} continue;
            }
            auto hi=std::upper_bound(samples.begin(),samples.end(),along,[](float v,const auto &p){return v<p.first;});
            if(hi==samples.end()||hi==samples.begin()) continue;
            auto lo=hi-1;
            float f=(along-lo->first)/(hi->first-lo->first);
            float offset=lo->second+(hi->second-lo->second)*f;
            float desired=offset*(1-float(toolStrength_));
            float original=across;
            if(across<=desired) original=-radius+(across+radius)*(offset+radius)/(desired+radius);
            else original=offset+(across-desired)*(radius-offset)/(radius-desired);
            auto direction=(a*std::cos(along)+t*std::sin(along))*std::cos(original)+n*std::sin(original);
            auto color=QColor::fromRgb(SphericalRaster::sampleDirection(source,direction));
            editLayer_.setPixelColor(x,y,color); composite_.setPixelColor(x,y,color);
            if(start<0)start=x;
        }
        if(start>=0)dirty+=QRect(start,y,source.width()-start,1);
    }
    partialTextureDirty_+=dirty; revision_=nextRevision_++; setModified(revision_!=savedRevision_);
    resetLine(); emit statusMessage(tr("Линия выпрямлена. Ctrl+Z — отменить."));
}

void SphereView::showPole(int pole) {
    if (!poleView_) { savedPitch_=pitch_; savedFov_=fov_; }
    poleView_=pole; pitch_=90.f*float(pole); fov_=90.f;
    cursorInside_=false; update();
    emit statusMessage(pole>0?tr("Зенит: сферические инструменты доступны, перетаскивание вращает вид."):tr("Надир: сферические инструменты доступны, перетаскивание вращает вид."));
}
void SphereView::showZenith(){showPole(1);}
void SphereView::showNadir(){showPole(-1);}
void SphereView::showHorizon(){
    pitch_=poleView_?savedPitch_:0.f;
    if(poleView_) fov_=savedFov_;
    poleView_=0; cursorInside_=false; update();
}
void SphereView::repairPole(){
    if(!poleView_ || !hasImage()){emit statusMessage(tr("Сначала выберите зенит или надир."));return;}
    if(toolStrength_<=0) return;
    const QImage source=composite_;
    const float radius=float(brushRadius_)*rad;
    const float strength=float(toolStrength_);
    const int w=source.width(),h=source.height();
    const int rows=std::min(h,int(std::ceil(radius*h/PI)));
    if(rows<1)return;
    pushUndo();
    // Blend azimuthal neighbours near the singularity; keep the outer cap unchanged.
    for(int j=0;j<rows;++j){
        int y=poleView_>0?j:h-1-j;
        float theta=float(PI)*(j+0.5f)/h;
        float f=clampf(1-theta/radius,0,1);
        float weight=strength*f*f*(3-2*f);
        if(weight<=0)continue;
        for(int x=0;x<w;++x){
            float rr=0,gg=0,bb=0;
            constexpr int taps=16;
            for(int k=0;k<taps;++k){
                float shift=(float(k)/(taps-1)-0.5f)*f*w;
                int sx=(x+int(std::round(shift))+w)%w;
                auto c=source.pixelColor(sx,y);rr+=c.redF();gg+=c.greenF();bb+=c.blueF();
            }
            auto c=source.pixelColor(x,y);
            auto out=QColor::fromRgbF(c.redF()*(1-weight)+rr/taps*weight,c.greenF()*(1-weight)+gg/taps*weight,c.blueF()*(1-weight)+bb/taps*weight,1);
            editLayer_.setPixelColor(x,y,out);composite_.setPixelColor(x,y,out);
        }
    }
    partialTextureDirty_+=QRect(0,poleView_>0?0:h-rows,w,rows);
    revision_=nextRevision_++;setModified(revision_!=savedRevision_);update();
    emit statusMessage(tr("Полюс сглажен. Ctrl+Z — отменить."));
}

void SphereView::smartSelect(const QVector3D &center,int operation){
    const auto u=QVector3D::crossProduct(std::abs(center.y())<0.9f?QVector3D(0,1,0):QVector3D(1,0,0),center).normalized();
    const auto v=QVector3D::crossProduct(center,u).normalized();
    const float extent=std::tan(std::clamp(fov_*0.5f,15.f,55.f)*rad);
    const QImage source=composite_;
    const auto request=++selectionRequest_,doc=documentId_,rev=revision_;
    const QString path=QCoreApplication::applicationDirPath()+"/models/sam_point.onnx";
    auto *watcher=new QFutureWatcher<AiEngine::Result>(this);
    connect(watcher,&QFutureWatcher<AiEngine::Result>::finished,this,[=,this]{
        auto result=watcher->result();watcher->deleteLater();
        if(request!=selectionRequest_||doc!=documentId_||rev!=revision_)return;
        if(!result.error.isEmpty()||result.image.isNull()){emit statusMessage(result.error);return;}
        if(operation==0||aiMask_.isNull()){aiMask_=QImage(source.size(),QImage::Format_Grayscale8);aiMask_.fill(0);}
        const auto mask=result.image;
        for(int y=0;y<source.height();++y)for(int x=0;x<source.width();++x){
            auto d=SphericalRaster::pixelDirection(x,y,source.width(),source.height());
            float z=QVector3D::dotProduct(d,center);if(z<=0)continue;
            float px=QVector3D::dotProduct(d,u)/z/extent,py=QVector3D::dotProduct(d,v)/z/extent;
            if(std::abs(px)>=1||std::abs(py)>=1)continue;
            int mx=std::clamp(int((px+1)*0.5f*mask.width()),0,mask.width()-1);
            int my=std::clamp(int((1-py)*0.5f*mask.height()),0,mask.height()-1);
            if(mask.constScanLine(my)[mx]>127)aiMask_.scanLine(y)[x]=operation<0?0:255;
        }
        hasAiSelection_=true;fullMaskDirty_=true;update();emit statusMessage(tr("Умное выделение завершено."));
    });
    emit statusMessage(tr("SAM: выделение объекта…"));
    watcher->setFuture(QtConcurrent::run([=]{
        QImage patch(1024,1024,QImage::Format_RGB32);
        for(int y=0;y<1024;++y)for(int x=0;x<1024;++x){
            auto d=(center+extent*((2.f*(x+0.5f)/1024-1)*u+(1-2.f*(y+0.5f)/1024)*v)).normalized();
            patch.setPixel(x,y,SphericalRaster::sampleDirection(source,d));
        }
        return AiEngine::segment(path,patch);
    }));
}

void SphereView::showSeam(){
    showHorizon();pitch_=0; yaw_=180;cursorInside_=false;update();
}
void SphereView::smartSeam(){
    if(!hasImage()||toolStrength_<=0)return;
    pushUndo();
    auto dirty=SphericalAI::smartSeam(editLayer_,composite_,std::max(2,int(brushRadius_*baseImage_.width()/360)),toolStrength_);
    if(dirty.isEmpty())return;
    partialTextureDirty_+=dirty;revision_=nextRevision_++;setModified(revision_!=savedRevision_);
    showSeam();emit statusMessage(tr("Умный шов: совмещение и цветокоррекция. Ctrl+Z — отменить."));
}
void SphereView::selectSeamForAi(){
    if(!hasImage())return;
    setMode(Mode::AiInpaint);
    clearAiSelection();
    aiMask_=QImage(baseImage_.size(),QImage::Format_Grayscale8);aiMask_.fill(0);
    const int half=std::clamp(int(brushRadius_*baseImage_.width()/360),1,std::max(1,std::min(256,baseImage_.width()/4)));
    const int height=std::min(768,baseImage_.height());
    const int top=(baseImage_.height()-height)/2;
    for(int y=top;y<top+height;++y)for(int d=0;d<half;++d){
        aiMask_.scanLine(y)[d]=255;aiMask_.scanLine(y)[baseImage_.width()-1-d]=255;
    }
    hasAiSelection_=true;fullMaskDirty_=true;showSeam();
    emit statusMessage(tr("Центральный участок шва выделен. Нажмите «Применить ИИ»."));
}

void SphereView::previewSeam(){
    if(!hasImage()||toolStrength_<=0)return;
    QImage layer=editLayer_, image=composite_;
    auto dirty=SphericalAI::smartSeam(layer,image,std::max(2,int(brushRadius_*baseImage_.width()/360)),toolStrength_);
    if(dirty.isEmpty())return;
    showSeam();
    // Modal comparison keeps editing and saving disabled until a decision.
    QImage original=composite_;
    QMessageBox dialog(QMessageBox::Question,tr("Предпросмотр шва"),tr("Сравните результат на сфере."),QMessageBox::NoButton,this);
    auto *toggle=dialog.addButton(tr("До / После"),QMessageBox::ActionRole);
    auto *apply=dialog.addButton(tr("Применить"),QMessageBox::AcceptRole);
    dialog.addButton(tr("Отмена"),QMessageBox::RejectRole);
    bool after=true;
    composite_=image;fullTextureDirty_=true;update();
    connect(toggle,&QAbstractButton::clicked,&dialog,[&]{after=!after;composite_=after?image:original;fullTextureDirty_=true;update();});
    dialog.exec();
    composite_=original;fullTextureDirty_=true;
    if(dialog.clickedButton()==apply){
        pushUndo();editLayer_=layer;composite_=image;revision_=nextRevision_++;setModified(revision_!=savedRevision_);
    }
    update();
}

void SphereView::diagnosePanorama(){
    if(!hasImage())return;
    clearDiagnostics();
    const QImage img=composite_.scaled(1024,512,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    struct Issue{QString name;int x,y;double score;};
    QVector<Issue> issues;
    auto gray=[&](int x,int y){return qGray(img.pixel((x+1024)%1024,std::clamp(y,0,511)));};
    for(int y=16;y<512;y+=32){
        double seam=0,normal=0;int n=0;
        for(int j=std::max(0,y-16);j<std::min(512,y+16);++j){
            seam+=std::abs(gray(0,j)-gray(1023,j));
            normal+=(std::abs(gray(0,j)-gray(1,j))+std::abs(gray(1023,j)-gray(1022,j)))*0.5;
            ++n;
        }
        if(seam/n>18 && seam>2.5*(normal+n))issues.push_back({tr("Перепад на шве"),0,y,seam/n});
    }
    for(int y=32;y<480;y+=64)for(int x=32;x<1024;x+=64){
        double edge=0,lap=0;int n=0;
        for(int j=y-24;j<y+24;j+=2)for(int i=x-24;i<x+24;i+=2){
            int c=gray(i,j);
            edge+=std::abs(gray(i+1,j)-gray(i-1,j))+std::abs(gray(i,j+1)-gray(i,j-1));
            lap+=std::abs(4*c-gray(i+1,j)-gray(i-1,j)-gray(i,j+1)-gray(i,j-1));++n;
        }
        if(edge/n>4 && lap/(edge+1)<0.18)issues.push_back({tr("Возможное размытие"),x,y,edge/(lap+1)});
    }
    // Estimate a dominant sinusoidal edge near the equator. A tilted
    // great circle projects approximately as a sinusoid for small tilts.
    double aa=0,bb=0,energy=0;int valid=0;
    for(int x=8;x<1024;x+=16){
        int bestY=256,best=0;
        for(int y=192;y<=320;++y){
            int g=std::abs(gray(x,y+2)-gray(x,y-2));
            if(g>best){best=g;bestY=y;}
        }
        if(best<35)continue;
        double angle=x*360.0/1024*rad;
        aa+=(bestY-256)*std::sin(angle);bb+=(bestY-256)*std::cos(angle);
        energy+=(bestY-256)*(bestY-256);++valid;
    }
    if(valid>=40){
        aa*=2.0/valid;bb*=2.0/valid;
        double amplitude=std::hypot(aa,bb);
        if(amplitude>9 && amplitude*amplitude*valid/2>energy*0.65){
            for(int x=128;x<1024;x+=256){
                double angle=x*360.0/1024*rad;
                issues.push_back({tr("Возможный наклон горизонта"),x,int(256+aa*std::sin(angle)+bb*std::cos(angle)),amplitude*180/512});
            }
        }
    }
    // Compare directional gradients after compensating for spherical
    // longitude compression. This is a heuristic, not object recognition.
    for(int y: {16,32,479,495}){
        double latitude=(90-y*180.0/512)*rad;
        int dx=std::clamp(int(std::lround(1/std::max(0.08,std::cos(latitude)))),1,12);
        for(int x=64;x<1024;x+=128){
            double gx=0,gy=0;int n=0;
            for(int j=y-6;j<=y+6;j+=2)for(int i=x-48;i<=x+48;i+=4){
                gx+=std::abs(gray(i+dx,j)-gray(i-dx,j));
                gy+=std::abs(gray(i,j+1)-gray(i,j-1));++n;
            }
            if(gy/n>8 && gx<gy*0.22)issues.push_back({tr("Возможное растяжение полюса"),x,y,gy/(gx+1)});
        }
    }
    diagnosticDocument_=documentId_;diagnosticRevision_=revision_;
    for(const auto &i:issues){
        QColor color= i.name==tr("Перепад на шве")?QColor(255,80,70):
            i.name==tr("Возможное размытие")?QColor(255,190,40):
            i.name==tr("Возможный наклон горизонта")?QColor(70,210,255):QColor(220,100,255);
        diagnosticMarks_.push_back({SphericalRaster::pixelDirection(i.x,i.y,1024,512),color,i.name});
    }
    update();
    if(issues.isEmpty()){emit statusMessage(tr("Диагностика: явных перепадов шва и подозрительного размытия не найдено."));return;}
    QStringList choices;
    for(const auto &i:issues)choices<<tr("%1 — долгота %2°, широта %3°").arg(i.name).arg(i.x*360.0/1024-180,0,'f',1).arg(90-i.y*180.0/512,0,'f',1);
    bool ok=false;
    QString choice=QInputDialog::getItem(this,tr("Диагностика панорамы"),tr("Подозрительные области (эвристическая оценка). Выберите область для просмотра:"),choices,0,false,&ok);
    if(!ok)return;
    int index=choices.indexOf(choice);if(index<0)return;
    const auto i=issues[index];
    showHorizon();yaw_=180.f-i.x*360.f/1024;pitch_=i.y*180.f/512-90.f;fov_=55;update();
    emit statusMessage(i.name);
}

void SphereView::clearDiagnostics(){ diagnosticMarks_.clear(); update(); }

bool SphereView::prepareObjectRemoval(){
    if(!hasAiSelection_||aiMask_.isNull())return false;
    // Expand the mask slightly to include antialiased object boundaries.
    const QImage original=aiMask_;
    const int w=original.width(),h=original.height();
    const int radius=std::clamp(w/1024,2,12);
    QImage expanded=original;
    for(int y=0;y<h;++y){
        auto *dst=expanded.scanLine(y);
        const auto *src=original.constScanLine(y);
        for(int x=0;x<w;++x)if(src[x]>127){
            for(int d=-radius;d<=radius;++d)dst[(x+d+w)%w]=255;
        }
    }
    const QImage horizontal=expanded;
    for(int y=0;y<h;++y){
        auto *dst=expanded.scanLine(y);
        for(int j=std::max(0,y-radius);j<=std::min(h-1,y+radius);++j){
            const auto *src=horizontal.constScanLine(j);
            for(int x=0;x<w;++x)dst[x]=std::max(dst[x],src[x]);
        }
    }
    ++selectionRequest_;
    aiMask_=expanded;fullMaskDirty_=true;
    setMode(Mode::AiInpaint);
    update();return true;
}

void SphereView::autoLevelHorizon(){
    if(!hasImage())return;
    const QImage small=composite_.scaled(1024,512,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    // Fit a great-circle plane to strong near-equatorial horizontal edges.
    struct Point { double a,b,z; };
    std::vector<Point> points;
    for(int x=8;x<1024;x+=16){
        int best=0,by=256;
        for(int y=170;y<=342;++y){
            int g=0;
            for(int dx=-3;dx<=3;++dx)
                g+=std::abs(qGray(small.pixel(x+dx,y+2))-qGray(small.pixel(x+dx,y-2)));
            if(g>best){best=g;by=y;}
        }
        if(best<245)continue;
        double lon=(x+0.5)*2*PI/1024-PI;
        double lat=PI/2-(by+0.5)*PI/512;
        points.push_back({std::sin(lon),std::cos(lon),std::tan(lat)});
    }
    double a=0,b=0;
    int count=0;
    for(int pass=0;pass<4;++pass){
        double aa=0,ab=0,bb=0,az=0,bz=0;count=0;
        for(const auto &p:points){
            if(pass && std::abs(p.z-a*p.a-b*p.b)>0.065)continue;
            aa+=p.a*p.a;ab+=p.a*p.b;bb+=p.b*p.b;az+=p.a*p.z;bz+=p.b*p.z;++count;
        }
        double det=aa*bb-ab*ab;
        if(count<24||det<1e-6)break;
        a=(az*bb-bz*ab)/det;b=(bz*aa-az*ab)/det;
    }
    if(count<24||count<double(points.size())*0.65){
        QMessageBox::information(this,tr("Горизонт"),tr("Не удалось надёжно определить горизонт. Панорама не изменена."));return;
    }
    QVector3D normal(float(-a),1.f,float(b));normal.normalize();
    double tilt=std::acos(std::clamp(double(normal.y()),-1.0,1.0))/rad;
    if(tilt<0.25){emit statusMessage(tr("Заметного наклона горизонта не найдено."));return;}
    if(tilt>25){emit statusMessage(tr("Слишком большой или недостоверный наклон. Коррекция отменена."));return;}
    bool ok=false;
    double percent=QInputDialog::getDouble(this,tr("Выравнивание горизонта"),
        tr("Оценка наклона: %1°. Сила коррекции (%):").arg(tilt,0,'f',1),100,0,100,0,&ok);
    if(!ok||percent<=0)return;
    const QQuaternion rotation=QQuaternion::slerp(QQuaternion(),QQuaternion::rotationTo(QVector3D(0,1,0),normal),float(percent/100));
    const QImage original=composite_;
    QImage result(original.size(),QImage::Format_RGBA8888);
    if(result.isNull())return;
    for(int y=0;y<result.height();++y)for(int x=0;x<result.width();++x){
        auto d=SphericalRaster::pixelDirection(x,y,result.width(),result.height());
        result.setPixelColor(x,y,QColor::fromRgba(SphericalRaster::sampleDirection(original,rotation.rotatedVector(d))));
    }
    QMessageBox dialog(QMessageBox::Question,tr("Горизонт"),tr("Проверьте результат на сфере."),QMessageBox::NoButton,this);
    auto *toggle=dialog.addButton(tr("До / После"),QMessageBox::ActionRole);
    auto *apply=dialog.addButton(tr("Применить"),QMessageBox::AcceptRole);
    dialog.addButton(tr("Отмена"),QMessageBox::RejectRole);
    bool after=true;
    composite_=result;fullTextureDirty_=true;update();
    connect(toggle,&QPushButton::clicked,&dialog,[&]{after=!after;composite_=after?result:original;fullTextureDirty_=true;update();});
    dialog.exec();
    composite_=original;fullTextureDirty_=true;
    if(dialog.clickedButton()==apply){
        pushUndo();editLayer_=result;composite_=result;
        clearAiSelection();clearDiagnostics();resetMesh();roundCenter_.reset();roundContour_.clear();linePoints_.clear();
        revision_=nextRevision_++;setModified(revision_!=savedRevision_);
    }
    update();
}

void SphereView::localColorCorrection(){
    if(!hasImage()||!hasAiSelection_||aiMask_.isNull()){
        QMessageBox::information(this,tr("Цветокоррекция"),tr("Сначала выделите объект умным выделением или кистью ИИ."));return;
    }
    const QImage original=composite_, mask=aiMask_;
    const bool showMask=hasAiSelection_;
    ++selectionRequest_;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Локальная цветокоррекция"));
    auto *layout=new QFormLayout(&dialog);
    QSlider *sliders[6];
    const QString names[]={tr("Яркость"),tr("Контраст"),tr("Насыщенность"),tr("Температура"),tr("Тени"),tr("Света")};
    for(int i=0;i<6;++i){
        sliders[i]=new QSlider(Qt::Horizontal,&dialog);
        sliders[i]->setRange(-100,100);sliders[i]->setValue(0);
        layout->addRow(names[i],sliders[i]);
    }
    auto *before=new QCheckBox(tr("Показать оригинал"),&dialog);
    layout->addRow(before);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    layout->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QImage corrected=original;
    auto render=[&]{
        corrected=original;
        float v[6];for(int i=0;i<6;++i)v[i]=sliders[i]->value()/100.f;
        for(int y=0;y<original.height();++y){
            const auto *m=mask.constScanLine(y);
            for(int x=0;x<original.width();++x){
                if(!m[x])continue;
                auto c=original.pixelColor(x,y);
                float rgb[]={float(c.redF()),float(c.greenF()),float(c.blueF())};
                float lum=0.2126f*rgb[0]+0.7152f*rgb[1]+0.0722f*rgb[2];
                float tone=v[0]*0.25f+v[4]*0.35f*(1-lum)*(1-lum)+v[5]*0.35f*lum*lum;
                float weight=m[x]/255.f;
                for(int k=0;k<3;++k){
                    float z=(rgb[k]-lum)*(1+v[2])+lum;
                    z=(z-0.5f)*(1+v[1])+0.5f+tone;
                    z+=v[3]*0.12f*(k==0?1.f:k==2?-1.f:0.f);
                    rgb[k]+=weight*(std::clamp(z,0.f,1.f)-rgb[k]);
                }
                corrected.setPixelColor(x,y,QColor::fromRgbF(rgb[0],rgb[1],rgb[2],c.alphaF()));
            }
        }
        composite_=before->isChecked()?original:corrected;fullTextureDirty_=true;update();
    };
    const bool gpuPreview=isValid() && shader_ && shader_->isLinked();
    auto preview=[&]{
        if(!gpuPreview){render();return;}
        for(int i=0;i<6;++i)colorValues_[i]=sliders[i]->value()/100.f;
        colorPreview_=!before->isChecked();update();
    };
    for(auto *slider:sliders)connect(slider,&QSlider::valueChanged,&dialog,[&](int){preview();});
    connect(before,&QCheckBox::toggled,&dialog,[&](bool){preview();});
    hasAiSelection_=false;update();
    int decision=dialog.exec();
    colorPreview_=false;
    if(gpuPreview && decision==QDialog::Accepted)render();
    composite_=original;hasAiSelection_=showMask;fullTextureDirty_=true;
    if(decision==QDialog::Accepted && corrected!=original){
        pushUndo();
        for(int y=0;y<original.height();++y){
            const auto *m=mask.constScanLine(y);
            for(int x=0;x<original.width();++x)if(m[x]){
                auto c=corrected.pixelColor(x,y);
                if(c!=original.pixelColor(x,y))editLayer_.setPixelColor(x,y,c);
            }
        }
        composite_=corrected;revision_=nextRevision_++;setModified(revision_!=savedRevision_);
        clearAiSelection();
    }
    update();
}

void SphereView::roundFromSelection(){
    if(!hasImage()||!hasAiSelection_||aiMask_.isNull()){
        emit statusMessage(tr("Сначала выделите объект кистью или MobileSAM."));return;
    }
    bool ok=false;
    const QStringList options={QStringLiteral("8"),QStringLiteral("16"),QStringLiteral("32"),QStringLiteral("64"),QStringLiteral("128")};
    int n=QInputDialog::getItem(this,tr("Округление по выделению"),tr("Количество точек контура:"),options,2,false,&ok).toInt();
    if(!ok||n<8)return;
    const QImage mask=aiMask_.scaled(2048,1024,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    const int w=mask.width(),h=mask.height();
    QVector3D sum;double weight=0;
    for(int y=0;y<h;++y){
        float area=std::sin(PI*(y+0.5f)/h);
        const auto *row=mask.constScanLine(y);
        for(int x=0;x<w;++x)if(row[x]>127){
            sum+=SphericalRaster::pixelDirection(x,y,w,h)*area;weight+=area;
        }
    }
    if(weight<1||sum.length()/weight<0.65){
        emit statusMessage(tr("Выделение пустое или слишком большое для округления."));return;
    }
    const QVector3D c=sum.normalized();
    const QVector3D u=QVector3D::crossProduct(std::abs(c.y())<0.9f?QVector3D(0,1,0):QVector3D(1,0,0),c).normalized();
    const QVector3D v=QVector3D::crossProduct(c,u).normalized();
    std::vector<double> radii(n,0.0);
    for(int y=0;y<h;++y){
        const auto *row=mask.constScanLine(y);
        for(int x=0;x<w;++x)if(row[x]>127){
            if(row[(x+w-1)%w]>127 && row[(x+1)%w]>127 &&
                mask.constScanLine(std::max(0,y-1))[x]>127 &&
                mask.constScanLine(std::min(h-1,y+1))[x]>127)continue;
            auto d=SphericalRaster::pixelDirection(x,y,w,h);
            double r=std::acos(clampf(QVector3D::dotProduct(c,d),-1,1));
            if(r>PI/3){emit statusMessage(tr("Объект слишком велик для радиального округления."));return;}
            double a=std::atan2(QVector3D::dotProduct(d,v),QVector3D::dotProduct(d,u));
            int k=(int(std::lround(a*n/(2*PI)))%n+n)%n;
            radii[k]=std::max(radii[k],r);
        }
    }
    int populated=0;for(double r:radii)if(r>0.001)++populated;
    if(populated<n/2){emit statusMessage(tr("Недостаточно точек контура. Выберите меньше точек или уточните маску."));return;}
    const auto raw=radii;
    for(int k=0;k<n;++k)if(radii[k]<=0.001){
        int l=1,r=1;while(raw[(k+n-l)%n]<=0.001)++l;while(raw[(k+r)%n]<=0.001)++r;
        radii[k]=(raw[(k+n-l)%n]*r+raw[(k+r)%n]*l)/(l+r);
    }
    const auto filled=radii;
    setMode(Mode::RoundObject);
    roundCenter_=c;
    for(int k=0;k<n;++k){
        double a=k*2*PI/n;
        double r=(filled[(k+n-1)%n]+2*filled[k]+filled[(k+1)%n])/4;
        roundContour_.append((c*float(std::cos(r))+(u*float(std::cos(a))+v*float(std::sin(a)))*float(std::sin(r))).normalized());
    }
    clearAiSelection();update();
    emit statusMessage(tr("Центр и %1 точек готовы. Настройте силу и нажмите «Применить округление».").arg(n));
}

void SphereView::manualLevelHorizon(){
    if(!hasImage()||levelPreview_)return;
    setMode(Mode::Navigate);showHorizon();
    const QImage original=composite_;
    const quint64 doc=documentId_,rev=revision_;
    const bool maskVisible=hasAiSelection_;hasAiSelection_=false;
    ++selectionRequest_;
    auto *dialog=new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Ручное выравнивание сферы"));
    auto *layout=new QFormLayout(dialog);
    QSlider *sliders[3];
    const QString labels[]={tr("Наклон X"),tr("Поворот Y"),tr("Крен Z")};
    for(int i=0;i<3;++i){
        sliders[i]=new QSlider(Qt::Horizontal,dialog);sliders[i]->setRange(-1800,1800);
        sliders[i]->setValue(0);layout->addRow(labels[i],sliders[i]);
    }
    auto *reset=new QPushButton(tr("Сбросить"),dialog);layout->addRow(reset);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,dialog);layout->addRow(buttons);
    levelRotation_=QQuaternion();levelPreview_=true;update();
    auto refresh=[this,x=sliders[0],y=sliders[1],z=sliders[2]]{
        levelRotation_=QQuaternion::fromEulerAngles(x->value()/10.f,y->value()/10.f,z->value()/10.f);update();
    };
    for(auto *slider:sliders)connect(slider,&QSlider::valueChanged,dialog,[refresh](int){refresh();});
    connect(reset,&QPushButton::clicked,dialog,[x=sliders[0],y=sliders[1],z=sliders[2]]{x->setValue(0);y->setValue(0);z->setValue(0);});
    connect(buttons,&QDialogButtonBox::accepted,dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    connect(dialog,&QDialog::finished,this,[=,this](int decision){
        auto rotation=levelRotation_;levelPreview_=false;
        if(doc!=documentId_||rev!=revision_){update();return;}
        hasAiSelection_=maskVisible;
        if(decision==QDialog::Accepted && !rotation.isIdentity()){
            QImage result(original.size(),QImage::Format_RGBA8888);
            if(!result.isNull()){
                for(int y=0;y<result.height();++y)for(int x=0;x<result.width();++x){
                    auto d=SphericalRaster::pixelDirection(x,y,result.width(),result.height());
                    result.setPixelColor(x,y,QColor::fromRgba(SphericalRaster::sampleDirection(original,rotation.rotatedVector(d))));
                }
                pushUndo();editLayer_=result;composite_=result;fullTextureDirty_=true;
                revision_=nextRevision_++;setModified(revision_!=savedRevision_);
                clearAiSelection();clearDiagnostics();
                emit statusMessage(tr("Ориентация сферы применена. Сохраните панораму."));
            }
        }
        update();
    });
    dialog->show();
}
