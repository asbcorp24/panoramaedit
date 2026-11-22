# PanoramaEdit

Native Windows editor for equirectangular 360° × 180° panoramas (2:1), based on C++20, Qt 6 Widgets and OpenGL.

## First milestone

- Explore from inside a 360° sphere with mouse drag and mouse wheel zoom.
- Load panorama JPG/PNG/BMP (recommended 2:1).
- Paint directly on the sphere with a geodesic brush, including the 0°/360° seam.
- Brush colour and angular radius controls.
- Undo/redo and save as PNG or JPEG.
- Windows build artifacts from GitHub Actions.

## Build (Windows)

Install Visual Studio 2022 with C++ workload, CMake >= 3.24 and Qt 6.x (MSVC 2022 64-bit, OpenGLWidgets module). Set `CMAKE_PREFIX_PATH` to your Qt installation.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build --config Release
```

For deploying Qt DLLs next to the executable, run Qt's `windeployqt` on `build/Release/PanoramaEdit.exe` (or use the GitHub Actions workflow which handles this).

## Controls

- **Open**: Ctrl+O; **Save As**: Ctrl+Shift+S.
- **Look around**: left mouse drag in Navigate mode, right mouse drag in either mode.
- **Paint**: left mouse drag in Brush mode; painting is applied to original equirectangular texture.
- **Zoom**: mouse wheel.
- **Undo/Redo**: Ctrl+Z / Ctrl+Y.

This is the initial usable editor core, **not** a Photoshop feature-complete implementation. Layers, masks, clone stamp, healing and tiled 16K engine are future milestones.

## Geometry

Pixels remain a standard equirectangular 2:1 image, but brush footprints are calculated by angular distance between spherical directions, not flat screen pixels. Longitude wraps seamlessly at both edges; latitude remains clamped at poles. The sphere is rendered from its centre with inward-facing texture.
