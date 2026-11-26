#include "GpuImageProcessor.h"

#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPainter>

#include <algorithm>
#include <array>

namespace GpuImageProcessor {
namespace {

const char *vertexShader = R"(#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char *fragmentShader = R"(#version 330 core
in vec2 vUV;
out vec4 outColor;
uniform sampler2D uSource;
uniform sampler2D uMask;
uniform bool uUseMask;
uniform vec4 uLevels;       // black, white, gamma, brightness
uniform vec4 uTone;         // contrast, saturation, temperature, shadows
uniform vec4 uChannels;     // red, green, blue, highlights
void main() {
    vec4 original = texture(uSource, vUV);
    vec3 rgb = original.rgb;
    vec3 z = pow(clamp((rgb - uLevels.x) /
                      max(0.004, uLevels.y - uLevels.x), 0.0, 1.0),
                 vec3(1.0 / max(0.01, uLevels.z)));
    float l = dot(z, vec3(0.2126, 0.7152, 0.0722));
    z = (z - l) * (1.0 + uTone.y) + l;
    z = (z - 0.5) * (1.0 + uTone.x) + 0.5 + uLevels.w * 0.25;
    z += vec3(uChannels.x, uChannels.y, uChannels.z) * 0.2;
    z += uTone.z * 0.12 * vec3(1.0, 0.0, -1.0);
    z += uTone.w * 0.35 * (1.0 - l) * (1.0 - l);
    z += uChannels.w * 0.35 * l * l;
    float amount = uUseMask ? texture(uMask, vUV).r : 1.0;
    outColor = vec4(mix(rgb, clamp(z, 0.0, 1.0), amount), original.a);
}
)";

bool uploadTexture(QOpenGLFunctions_3_3_Core *gl, GLuint *texture,
                   const QImage &image, GLenum internalFormat,
                   GLenum format) {
    gl->glGenTextures(1, texture);
    gl->glBindTexture(GL_TEXTURE_2D, *texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, image.bytesPerLine() / (format == GL_RGBA ? 4 : 1));
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GLint(internalFormat),
                     image.width(), image.height(), 0, format,
                     GL_UNSIGNED_BYTE, image.constBits());
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return gl->glGetError() == GL_NO_ERROR;
}

} // namespace

bool GpuImageProcessor::applyGrade(const QImage &source, const QImage &mask,
                                  bool useMask, const Grade &grade,
                                  QImage *destination, QString *error,
                                  int preferredTileEdge) {
    const auto fail = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!destination || source.isNull())
        return fail(QStringLiteral("Исходное изображение или выходной буфер пуст."));
    if (useMask && (mask.isNull() || mask.size() != source.size()))
        return fail(QStringLiteral("Размер маски не совпадает с размером изображения."));

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context)
        return fail(QStringLiteral("Нет активного OpenGL-контекста."));
    if (!context->format().version() || context->format().majorVersion() < 3 ||
        (context->format().majorVersion() == 3 && context->format().minorVersion() < 3))
        return fail(QStringLiteral("Требуется OpenGL 3.3 или новее."));

    QOpenGLFunctions_3_3_Core gl;
    if (!gl.initializeOpenGLFunctions())
        return fail(QStringLiteral("Не удалось инициализировать OpenGL 3.3."));

    QOpenGLShaderProgram program;
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader) ||
        !program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader) ||
        !program.link())
        return fail(QStringLiteral("Ошибка компиляции GPU-шейдера: %1").arg(program.log()));

    QOpenGLVertexArrayObject vao;
    if (!vao.create())
        return fail(QStringLiteral("Не удалось создать OpenGL VAO."));

    GLint maxTextureSize = 0;
    gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (maxTextureSize < 64)
        return fail(QStringLiteral("Видеокарта сообщает недопустимый размер текстуры."));

    const int tileEdge = std::clamp(preferredTileEdge, 64, maxTextureSize);
    const QImage input = source.convertToFormat(QImage::Format_RGBA8888);
    const QImage maskImage = useMask ? mask.convertToFormat(QImage::Format_Grayscale8) : QImage();
    QImage output(input.size(), QImage::Format_RGBA8888);
    if (output.isNull())
        return fail(QStringLiteral("Недостаточно оперативной памяти для выходного изображения."));

    QOpenGLFramebufferObjectFormat fboFormat;
    fboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    fboFormat.setInternalTextureFormat(GL_RGBA8);

    GLint previousViewport[4]={0,0,0,0}, previousFramebuffer=0, previousActiveTexture=GL_TEXTURE0;
    gl.glGetIntegerv(GL_VIEWPORT, previousViewport);
    gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    gl.glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    const GLboolean depthWasEnabled=gl.glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blendWasEnabled=gl.glIsEnabled(GL_BLEND);
    GLuint sourceTexture = 0, maskTexture = 0;
    const auto cleanup = [&] {
        if (sourceTexture) gl.glDeleteTextures(1, &sourceTexture);
        if (maskTexture) gl.glDeleteTextures(1, &maskTexture);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFramebuffer));
        gl.glViewport(previousViewport[0],previousViewport[1],previousViewport[2],previousViewport[3]);
        if(depthWasEnabled)gl.glEnable(GL_DEPTH_TEST);else gl.glDisable(GL_DEPTH_TEST);
        if(blendWasEnabled)gl.glEnable(GL_BLEND);else gl.glDisable(GL_BLEND);
        gl.glActiveTexture(GLenum(previousActiveTexture));
        vao.destroy();
        program.release();
    };

    program.bind();
    vao.bind();
    program.setUniformValue("uSource", 0);
    program.setUniformValue("uMask", 1);
    program.setUniformValue("uUseMask", useMask);
    program.setUniformValue("uLevels", grade.blackPoint, grade.whitePoint,
                            grade.gamma, grade.brightness);
    program.setUniformValue("uTone", grade.contrast, grade.saturation,
                            grade.temperature, grade.shadows);
    program.setUniformValue("uChannels", grade.red, grade.green,
                            grade.blue, grade.highlights);

    for (int y = 0; y < input.height(); y += tileEdge) {
        const int tileHeight = std::min(tileEdge, input.height() - y);
        for (int x = 0; x < input.width(); x += tileEdge) {
            const int tileWidth = std::min(tileEdge, input.width() - x);
            const QRect rect(x, y, tileWidth, tileHeight);
            const QImage sourceTile = input.copy(rect);
            QImage maskTile = useMask ? maskImage.copy(rect) : QImage(tileWidth, tileHeight, QImage::Format_Grayscale8);
            if (!useMask) maskTile.fill(255);

            if (!uploadTexture(&gl, &sourceTexture, sourceTile, GL_RGBA8, GL_RGBA)) {
                cleanup();
                return fail(QStringLiteral("Не удалось загрузить тайл изображения в GPU."));
            }
            if (!uploadTexture(&gl, &maskTexture, maskTile, GL_R8, GL_RED)) {
                cleanup();
                return fail(QStringLiteral("Не удалось загрузить маску тайла в GPU."));
            }

            QOpenGLFramebufferObject fbo(tileWidth, tileHeight, fboFormat);
            if (!fbo.isValid() || !fbo.bind()) {
                cleanup();
                return fail(QStringLiteral("Не удалось создать GPU-буфер для тайла %1×%2.")
                            .arg(tileWidth).arg(tileHeight));
            }
            gl.glViewport(0, 0, tileWidth, tileHeight);
            gl.glDisable(GL_BLEND);
            gl.glDisable(GL_DEPTH_TEST);
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glBindTexture(GL_TEXTURE_2D, sourceTexture);
            gl.glActiveTexture(GL_TEXTURE1);
            gl.glBindTexture(GL_TEXTURE_2D, maskTexture);
            gl.glActiveTexture(GL_TEXTURE0);
            gl.glDrawArrays(GL_TRIANGLES, 0, 3);
            const GLenum drawError = gl.glGetError();
            QImage tileResult;
            if (drawError == GL_NO_ERROR)
                tileResult = fbo.toImage(false).convertToFormat(QImage::Format_RGBA8888);
            fbo.release();
            if (drawError != GL_NO_ERROR || tileResult.size() != rect.size()) {
                cleanup();
                return fail(QStringLiteral("Ошибка GPU при обработке тайла (%1, %2).").arg(x).arg(y));
            }
            QPainter painter(&output);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(rect.topLeft(), tileResult);
        }
    }

    cleanup();
    *destination = std::move(output);
    if (error) error->clear();
    return true;
}

} // namespace GpuImageProcessor
