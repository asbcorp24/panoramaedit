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
#include <QVector2D>
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
                      AiSharpen, AiInpaint, RoundObject, MeshWarp, Straighten, SmartSelect };
    explicit SphereView(QWidget *parent = nullptr);
    ~SphereView() override;
    bool openImage(const QString &path, QString *error = nullptr);
    bool saveImage(const QString &path);
    bool hasImage() const { return !baseImage_.isNull(); }
    void setMode(Mode mode);
    void setBrushColor(const QColor &color) { brushColor_ = color; }
    void setBrushRadius(double degrees);
    void setToolStrength(double value) { toolStrength_ = qBound(0.0, value, 1.0); update(); }
    void undo();
    void redo();
    Mode mode() const { return mode_; }
    quint64 documentId() const { return documentId_; }
    std::optional<SphericalAI::Patch> prepareAiPatch() const;
    bool applyAiResult(const SphericalAI::Patch &patch, const QImage &result);
    void clearAiSelection();
    void smoothSeam();
    void smartSeam();
    void previewSeam();
    void selectSeamForAi();
    void showSeam();
    void diagnosePanorama();
    void autoLevelHorizon();
    void manualLevelHorizon();
    void stretchPoleTexture();
    void localColorCorrection();
    void colorGrading();
    void clearDiagnostics();
    void roundObject();
    void roundFromSelection();
    void applyMesh();
    void resetMesh();
    void straightenLine();
    void resetLine();
    void showZenith();
    void showNadir();
    void showHorizon();
    void repairPole();
    bool prepareObjectRemoval();
    void smartSelect(const QVector3D &center, int operation);
    quint64 selectionRequest_=0;
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
    QVector<QVector3D> linePoints_;
    std::optional<QVector3D> meshCenter_;
    QVector3D meshU_, meshV_;
    float meshExtent_ = 0.2f;
    QVector<QVector2D> meshOffsets_;
    int meshDrag_ = -1;
    QVector2D meshStart_;
    struct DiagnosticMark { QVector3D direction; QColor color; QString label; };
    QVector<DiagnosticMark> diagnosticMarks_;
    quint64 diagnosticDocument_=0, diagnosticRevision_=0;
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
    bool poleStretchPreview_=false;
    float poleStretch_=0;
    bool levelPreview_=false;
    bool levelDragging_=false;
    QVector3D levelAnchor_;
    Q_SIGNAL void levelRotationChanged();
    QQuaternion levelRotation_;
    bool gradingPreview_=false, gradingMasked_=false;
    float grading_[12]={};
    bool colorPreview_=false;
    float colorValues_[6]={};
    bool fullTextureDirty_ = true;
    QRegion partialTextureDirty_;
    Mode mode_ = Mode::Navigate;
    QColor brushColor_{Qt::red};
    double brushRadius_ = 2.0;
    double toolStrength_ = 0.5;
    int poleView_ = 0;
    float savedPitch_ = 0, savedFov_ = 75;
    void showPole(int pole);
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
