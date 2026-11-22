#pragma once

#include <QIcon>

namespace ToolIcons {

enum class Kind {
    Navigate, Brush, Eraser, Stamp, Smudge, Push, Bloat, Pinch,
    Sharpen, Inpaint, Color, ApplyAi, ClearAi, Seam
};

// Resolution-independent artwork drawn into a high-DPI pixmap. No icon theme,
// SVG plugin or external image files are needed on Windows.
QIcon make(Kind kind);

} // namespace ToolIcons
