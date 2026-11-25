#!/usr/bin/env bash
set -Eeuo pipefail

echo "====================================================="
echo " PanoramaEdit - Linux dependency installer"
echo "====================================================="

if [[ "${EUID}" -eq 0 ]]; then
  SUDO=()
elif command -v sudo >/dev/null 2>&1; then
  SUDO=(sudo)
else
  echo "[ERROR] Run as root or install sudo."
  exit 1
fi

if [[ -r /etc/os-release ]]; then
  . /etc/os-release
else
  echo "[ERROR] Cannot identify Linux distribution (/etc/os-release missing)."
  exit 1
fi

echo "Detected: ${PRETTY_NAME:-unknown}"
echo

case "${ID:-}" in
  ubuntu|debian|linuxmint|pop)
    "${SUDO[@]}" apt-get update
    "${SUDO[@]}" apt-get install -y \
      build-essential cmake ninja-build git pkg-config curl ca-certificates \
      qt6-base-dev qt6-base-dev-tools libqt6opengl6-dev libgl1-mesa-dev mesa-utils
    ;;
  fedora)
    "${SUDO[@]}" dnf install -y \
      gcc-c++ make cmake ninja-build git pkgconf-pkg-config curl ca-certificates \
      qt6-qtbase-devel qt6-qtbase-gui-devel mesa-libGL-devel mesa-dri-drivers
    ;;
  arch|manjaro|endeavouros)
    "${SUDO[@]}" pacman -Sy --needed --noconfirm \
      base-devel cmake ninja git pkgconf curl ca-certificates \
      qt6-base mesa
    ;;
  opensuse*|opensuse-tumbleweed|opensuse-leap)
    "${SUDO[@]}" zypper --non-interactive refresh
    "${SUDO[@]}" zypper --non-interactive install \
      gcc-c++ make cmake ninja git pkg-config curl ca-certificates \
      libqt6-qtbase-devel libqt6-qtbase-devel-tools Mesa-libGL1
    ;;
  *)
    echo "[ERROR] Unsupported distribution: ${ID:-unknown}"
    echo "Install manually: C++20 compiler, CMake 3.24+, Qt 6.4 development"
    echo "packages (Widgets, OpenGL, OpenGLWidgets, Concurrent), pkg-config,"
    echo "Ninja (optional), Git, curl, OpenGL/Mesa development libraries."
    exit 1
    ;;
esac

echo
echo "Checking tool versions..."
cmake --version | head -n 1
c++ --version | head -n 1
if command -v ninja >/dev/null 2>&1; then ninja --version; fi

CMAKE_VERSION="$(cmake -E capabilities | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"]["string"])' 2>/dev/null || cmake --version | awk 'NR==1 {print $3}')"
if [[ "$(printf '%s\n' 3.24 "$CMAKE_VERSION" | sort -V | head -n1)" != "3.24" ]]; then
  echo "[ERROR] CMake 3.24 or newer is required; found $CMAKE_VERSION."
  exit 1
fi

if ! pkg-config --exists Qt6Widgets Qt6OpenGL Qt6OpenGLWidgets Qt6Concurrent; then
  echo "[ERROR] Qt 6 development modules are still missing."
  exit 1
fi

echo
echo "[SUCCESS] Linux build dependencies installed."
echo "Next: ./build_linux.sh"
echo
echo "Note: this installs native build dependencies only."
echo "Linux ONNX Runtime/AI acceleration is not yet configured in this project."
