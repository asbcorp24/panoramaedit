#!/usr/bin/env bash
set -Eeuo pipefail
cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build-linux}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

echo "====================================================="
echo " PanoramaEdit - Linux x86_64 Release build"
echo "====================================================="

for cmd in cmake c++ pkg-config; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "[ERROR] $cmd is missing."
    exit 1
  fi
done

if ! pkg-config --exists Qt6Widgets Qt6OpenGL Qt6OpenGLWidgets Qt6Concurrent; then
  echo "[ERROR] Qt 6 development packages were not found."
  echo "Ubuntu/Debian install command:"
  echo "  sudo apt update"
  echo "  sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-base-dev-tools libqt6opengl6-dev libgl1-mesa-dev"
  exit 1
fi

GENERATOR=()
if command -v ninja >/dev/null 2>&1; then
  GENERATOR=(-G Ninja)
fi

echo "[1/3] Configure CMake..."
cmake -S . -B "$BUILD_DIR" "${GENERATOR[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF

echo "[2/3] Compile..."
cmake --build "$BUILD_DIR" --parallel "$JOBS"

if [[ ! -x "$BUILD_DIR/PanoramaEdit" ]]; then
  echo "[ERROR] Build finished but executable was not found."
  exit 1
fi

echo "[3/3] Done."
echo "Executable: $BUILD_DIR/PanoramaEdit"
echo "Run: ./$BUILD_DIR/PanoramaEdit"
echo
echo "NOTE: Linux ONNX Runtime GPU/CPU integration is not yet configured."
echo "OpenGL acceleration is provided by the system graphics driver."
