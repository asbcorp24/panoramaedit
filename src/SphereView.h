#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QImage>
#include <QColor>
#include <QPointF>
#include <QVector3D>
#include <QQuaternion>
#include <QRegion>
#include "SphericalRaster.h"
#include "SphericalAI.h"
#include <memory>
#include <optional>

class QEnterEvent;

class SphereView final : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    enum class Mode { Navigate, Brush, Eraser, CloneStamp, Smudge, Push, Bloat, Pinch,
                      AiSharpen, AiInpaint, RoundObject };
    explicit SphereView(QWidget *parent = nullptr);
    ~SphereView() override;
    bool openImage(const QString &path, QString *error = nullptr);
    bool saveImage(const QString &path);
    bool hasImage() const { return !baseImage_.isNull(); }
    void setMode(Mode mode);
    void setBrushColor(const QColor &color) { brushColor_ = color; }
    void setBrushRadius(double degrees);
    void setToolStrength(double value) { toolStrength_ = value; }
    void undo();
    void redo();
    Mode mode() const { return mode_; }
    quint64 documentId() const { return documentId_; }
    std::optional<SphericalAI::Patch> prepareAiPatch() const;
    bool applyAiResult(const SphericalAI::Patch &patch, const QImage &result);
    void clearAiSelection();
    void smoothSeam();
    void roundObject();
    bool isModified() const { return modified_; }

signals:
    void imageChanged();
    void modifiedChanged(bool modified);
    void statusMessage(const QString &message);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void uploadTexture();
    void uploadMaskTexture();
    QVector3D directionFromScreen(const QPointF &pos) const;
    void paintAt(const QVector3D &direction);
    void strokeBetween(const QVector3D &a, const QVector3D &b);
    void pushUndo();
    void setModified(bool modified);
    void refreshComposite();
    std::optional<QVector3D> roundCenter_;
    QVector<QVector3D> roundContour_;
    QImage baseImage_;
    QImage editLayer_;
    QImage composite_;
    QImage cloneSnapshot_;
    QImage aiMask_;
    QRegion aiMaskDirty_;
    bool fullMaskDirty_ = true;
    bool hasAiSelection_ = false;
    std::unique_ptr<QOpenGLShaderProgram> shader_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer indexBuffer_{QOpenGLBuffer::IndexBuffer};
    QOpenGLVertexArrayObject vertexArray_;
    GLuint texture_ = 0;
    GLuint maskTexture_ = 0;
    GLsizei indexCount_ = 0;
    bool fullTextureDirty_ = true;
    QRegion partialTextureDirty_;
    Mode mode_ = Mode::Navigate;
    QColor brushColor_{Qt::red};
    double brushRadius_ = 2.0;
    double toolStrength_ = 0.5;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float fov_ = 75.0f;
    QVector3D cursorDirection_{0, 0, -1};
    bool cursorInside_ = false;
    QPoint lastMouse_;
    bool dragging_ = false;
    bool painting_ = false;
    std::optional<QVector3D> lastStroke_;
    std::optional<QVector3D> cloneSource_;
    QQuaternion cloneRotation_;
    struct EditState {
        QImage layer;
        quint64 revision = 0;
    };
    QVector<EditState> undoStack_;
    QVector<EditState> redoStack_;
    quint64 revision_ = 0;
    quint64 savedRevision_ = 0;
    quint64 nextRevision_ = 1;
    quint64 documentId_ = 0;
    bool strokeHasChanged_ = false;
    bool modified_ = false;
    static constexpr int historyLimit_ = 12;
};
