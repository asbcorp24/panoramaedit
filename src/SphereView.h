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
#include <memory>
#include <optional>

class SphereView final : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    enum class Mode { Navigate, Brush, Eraser, CloneStamp };
    explicit SphereView(QWidget *parent = nullptr);
    ~SphereView() override;
    bool openImage(const QString &path, QString *error = nullptr);
    bool saveImage(const QString &path);
    bool hasImage() const { return !baseImage_.isNull(); }
    void setMode(Mode mode);
    void setBrushColor(const QColor &color) { brushColor_ = color; }
    void setBrushRadius(double degrees) { brushRadius_ = degrees; }
    void undo();
    void redo();
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

private:
    void uploadTexture();
    QVector3D directionFromScreen(const QPointF &pos) const;
    void paintAt(const QVector3D &direction);
    void strokeBetween(const QVector3D &a, const QVector3D &b);
    void pushUndo();
    void setModified(bool modified);
    void refreshComposite();
    QImage baseImage_;
    QImage editLayer_;
    QImage composite_;
    QImage cloneSnapshot_;
    std::unique_ptr<QOpenGLShaderProgram> shader_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer indexBuffer_{QOpenGLBuffer::IndexBuffer};
    QOpenGLVertexArrayObject vertexArray_;
    GLuint texture_ = 0;
    GLsizei indexCount_ = 0;
    bool fullTextureDirty_ = true;
    QRegion partialTextureDirty_;
    Mode mode_ = Mode::Navigate;
    QColor brushColor_{Qt::red};
    double brushRadius_ = 2.0;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float fov_ = 75.0f;
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
    bool strokeHasChanged_ = false;
    bool modified_ = false;
    static constexpr int historyLimit_ = 12;
};
