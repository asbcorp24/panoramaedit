@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title PanoramaEdit - native offline AI models
echo ====================================================
echo   PanoramaEdit - native offline AI (no Python)
echo ====================================================
echo.
echo ONNX Runtime and DirectML DLLs are already bundled with the EXE.
echo This installer only downloads and verifies the two model files.
echo Existing compatible models will be kept.
echo.

if not exist "PanoramaEdit.exe" (
  echo [ERROR] PanoramaEdit.exe missing. Run this next to the EXE.
  goto :fail
)
if not exist "onnxruntime.dll" (
  echo [ERROR] onnxruntime.dll missing. Use the full new Windows ZIP.
  goto :fail
)
if not exist "models" mkdir "models"

if exist "models\deblur.onnx" (
  rem Broken 33 MB FP16 models were distributed in early builds.
  powershell -NoProfile -Command "if ((Get-Item 'models/deblur.onnx').Length -gt 60000000) { exit 0 } else { exit 1 }"
  if not errorlevel 1 (
    start /wait "" "%CD%\PanoramaEdit.exe" --check-ai sharpen "%CD%\models\deblur.onnx"
  )
  if errorlevel 1 (
    echo [FIX] Saving incompatible NAFNet model as a backup...
    ren "models\deblur.onnx" "deblur-incompatible-%RANDOM%.onnx"
    if errorlevel 1 goto :fail
  )
)

echo [1/2] NAFNet FP32 model, about 69 MB...
if not exist "models\deblur.onnx" (
  curl.exe -fL --retry 3 --output "models\deblur.onnx.part" "https://raw.githubusercontent.com/rock1110/nafnet-onnx/main/nafnet-width32-gopro-fp32.onnx"
  if errorlevel 1 goto :fail
  move /Y "models\deblur.onnx.part" "models\deblur.onnx" >nul
  if errorlevel 1 goto :fail
)
start /wait "" "%CD%\PanoramaEdit.exe" --check-ai sharpen "%CD%\models\deblur.onnx"
if errorlevel 1 (
  echo [ERROR] NAFNet model failed native ONNX validation.
  goto :fail
)

echo [2/2] LaMa inpainting model, about 208 MB...
if not exist "models\inpaint.onnx" (
  curl.exe -fL --retry 3 --output "models\inpaint.onnx.part" "https://huggingface.co/gumka/lama-onnx/resolve/main/lama_fp32.onnx?download=true"
  if errorlevel 1 goto :fail
  move /Y "models\inpaint.onnx.part" "models\inpaint.onnx" >nul
  if errorlevel 1 goto :fail
)
start /wait "" "%CD%\PanoramaEdit.exe" --check-ai inpaint "%CD%\models\inpaint.onnx"
if errorlevel 1 (
  echo [ERROR] LaMa model failed native ONNX validation.
  goto :fail
)

echo.
echo [SUCCESS] Both AI models are ready. No Python is needed.
echo You can now start PanoramaEdit.exe and use both AI tools offline.
pause
exit /b 0

:fail
echo.
echo [FAILED] Check the messages above. Painting still works without AI.
pause
exit /b 1
