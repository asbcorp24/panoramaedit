@echo off
setlocal
cd /d "%~dp0"
if "%~1"=="" (
  set "TARGET=%CD%\models"
) else (
  set "TARGET=%~1"
)
where py >nul 2>&1 || (echo Install Python 3.11 first. & exit /b 1)
if not exist ".sam-venv\Scripts\python.exe" py -3.11 -m venv .sam-venv
if errorlevel 1 exit /b 1
set "PY=.sam-venv\Scripts\python.exe"
"%PY%" -m pip install torch==2.5.1 torchvision==0.20.1 --index-url https://download.pytorch.org/whl/cpu
if errorlevel 1 exit /b 1
"%PY%" -m pip install "numpy<2" onnx onnxruntime timm git+https://github.com/ChaoningZhang/MobileSAM.git
if errorlevel 1 exit /b 1
if not exist "%TARGET%" mkdir "%TARGET%"
if not exist "%TARGET%\mobile_sam.pt" curl.exe -fL --retry 3 "https://raw.githubusercontent.com/ChaoningZhang/MobileSAM/master/weights/mobile_sam.pt" -o "%TARGET%\mobile_sam.pt"
if errorlevel 1 exit /b 1
"%PY%" scripts\export_mobile_sam.py --checkpoint "%TARGET%\mobile_sam.pt" --output "%TARGET%\sam_point.onnx"
if errorlevel 1 exit /b 1
echo Model ready. Place the models folder beside PanoramaEdit.exe.
