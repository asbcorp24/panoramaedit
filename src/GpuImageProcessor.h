#pragma once

#include <QImage>
#include <QString>

class QOpenGLContext;

namespace GpuImageProcessor {

// Runs inside an already-current OpenGL 3.3 context. The image is processed
// in bounded tiles so texture allocation never needs the full panorama size.
// Returns false with an explanatory error if the context or GPU pass fails.
class GpuImageProcessor final {
public:
    struct Grade {
        float blackPoint = 0.0f;
        float whitePoint = 1.0f;
        float gamma = 1.0f;
        float brightness = 0.0f;
        float contrast = 0.0f;
        float saturation = 0.0f;
        float temperature = 0.0f;
        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
        float shadows = 0.0f;
        float highlights = 0.0f;
        bool sphericalGradient = false;
        bool gradientByLatitude = false;
        float gradientCenterX = 0.0f;
        float gradientCenterY = 1.0f;
        float gradientCenterZ = 0.0f;
        float gradientInner = 0.15f;
        float gradientOuter = 0.8f;
        float gradientStrength = 0.0f;
        float gradientWarmth = 0.0f;
        bool panoramaAutoCorrection = false;
        float whiteBalance[3] = {1.0f, 1.0f, 1.0f};
        float sectorIllumination[16] = {1.0f};
        float seamDelta[3] = {0.0f, 0.0f, 0.0f};
        float seamWidth = 0.035f;
        float hdrStrength = 0.0f;
        float localContrast = 0.0f;
        float detailStrength = 0.0f;
    };

    static bool applyGrade(const QImage &source, const QImage &mask,
                           bool useMask, const Grade &grade,
                           QImage *destination, QString *error = nullptr,
                           int preferredTileEdge = 2048);
};

} // namespace GpuImageProcessor
