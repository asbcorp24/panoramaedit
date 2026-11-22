@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title PanoramaEdit - offline AI setup
echo ====================================================
echo   PanoramaEdit - one-time offline AI installation
echo ====================================================
echo.
echo This setup downloads Python packages and two model weights ONCE.
echo After setup, editing does not require the internet.
echo.

if not exist "ai_worker.py" (
  echo [ERROR] ai_worker.py not found. Run this script next to PanoramaEdit.exe.
  goto :fail
)
py -3 --version >nul 2>&1
if errorlevel 1 (
  echo [ERROR] Python 3 was not found.
  echo Install Python 3.11 or 3.12 from python.org, then run setup again.
  goto :fail
)
if not exist "ai_runtime\Scripts\python.exe" (
  py -3 -m venv "ai_runtime"
  if errorlevel 1 goto :fail
)
echo [1/3] Installing local AI inference libraries...
"ai_runtime\Scripts\python.exe" -m pip install --disable-pip-version-check --prefer-binary onnxruntime-directml numpy pillow
if errorlevel 1 goto :fail

if not exist "models" mkdir "models"
echo [2/3] Downloading NAFNet deblurring model, about 33 MB...
if not exist "models\deblur.onnx" (
  curl.exe -fL --retry 3 --output "models\deblur.onnx.part" "https://github.com/rock1110/nafnet-onnx/releases/download/v1/nafnet-width32-gopro-fp16.onnx"
  if errorlevel 1 goto :fail
  move /Y "models\deblur.onnx.part" "models\deblur.onnx" >nul
  if errorlevel 1 goto :fail
)
echo [3/3] Downloading LaMa inpainting model, about 208 MB...
if not exist "models\inpaint.onnx" (
  curl.exe -fL --retry 3 --output "models\inpaint.onnx.part" "https://huggingface.co/gumka/lama-onnx/resolve/main/lama_fp32.onnx?download=true"
  if errorlevel 1 goto :fail
  move /Y "models\inpaint.onnx.part" "models\inpaint.onnx" >nul
  if errorlevel 1 goto :fail
)
"ai_runtime\Scripts\python.exe" -c "from pathlib import Path; assert Path('models/deblur.onnx').stat().st_size > 20000000; assert Path('models/inpaint.onnx').stat().st_size > 150000000"
if errorlevel 1 goto :fail
echo.
echo [SUCCESS] The two AI modes are available without internet.
echo Restart PanoramaEdit if it is open.
pause
exit /b 0

:fail
echo.
echo [FAILED] AI installation is incomplete. Check the errors above.
echo You can continue using PanoramaEdit without AI.
pause
exit /b 1
