# PanoramaEdit

Native Windows editor for equirectangular 360° × 180° panoramas (2:1), based on C++20, Qt 6 Widgets and OpenGL.

## Панель инструментов

Интерфейс программы и названия инструментов переведены на русский язык.
Панель «Инструменты» при запуске **плавает над окном**: её можно перетаскивать
за заголовок или закрепить слева/справа. Если закрыли панель, верните её через
**Вид → Панель инструментов**.

**Радиус кисти** теперь можно задать вручную в числовом поле справа от ползунка:
введите значение от **0,1° до 30,0°** и нажмите Enter. Поле и ползунок всегда
показывают одно значение. Здесь используются *угловые градусы на сфере*, а не
пиксели эквиректангульной развёртки. Сила воздействия настраивается отдельным
ползунком от 1% до 100%.

Основные кнопки: «Навигация» (`N`), «Кисть» (`B`), «Ластик» (`E`),
«Штамп» (`S`), «Палец» (`F`), «Деформация» (`D`), «Вздутие» (`I`),
«Сжатие» (`C`). Для поворота сферы при выбранном инструменте используйте
правую кнопку мыши.

## Офлайн ИИ-восстановление (Windows)

В плавающей панели есть два инструмента:

- **ИИ-резкость (`U`)** — NAFNet-width32 для смазывания/нерезкости (надёжная FP32-модель ~69 МБ).
- **ИИ-дорисовка (`J`)** — LaMa для реконструкции текстур по окружению (~208 МБ).

**Подготовка один раз:** после сборки/скачивания Windows-пакета откройте папку
с `PanoramaEdit.exe` и запустите **`setup_ai_windows.bat`**. Понадобятся Python
3.11/3.12 и интернет *только на этапе установки*: скрипт создаст изолированное
окружение `ai_runtime`, установит ONNX Runtime DirectML, NumPy и Pillow,
скачает веса в `models/`. **Сама обработка полностью локальная и не отправляет
изображения в сеть.** Если модель или среда отсутствует, основной редактор
продолжает работать, а инструмент покажет инструкцию по установке.

Если использовали старую версию с NAFNet FP16 (~33 МБ) и видели ошибку
`Type parameter (T) ... tensor(float) and tensor(float16)`, повторно запустите
**обновлённый `setup_ai_windows.bat`** из новой сборки. Установщик сохранит
старый несовместимый файл под другим именем, скачает FP32 (~69 МБ) и **проверит,
что ONNX Runtime действительно открывает обе модели**. Не нужно заново
скачивать LaMa, если её файл уже исправен.

**Как использовать:** выберите один из двух инструментов, левой кнопкой
закрасьте область прямо на сфере (она подсвечивается голубым), затем нажмите
**«Применить ИИ к выделению»**. Ползунок «Сила воздействия» определяет долю
нового результата при наложении. Выделение можно сбросить кнопкой **«Очистить
ИИ-выделение»**, результат — `Ctrl+Z`. Во время работы модели интерфейс не
блокируется вычислениями: есть индикатор и отмена задачи. Для небольшого
расхода видеопамяти обрабатываются только фрагменты до **1024×1024** с
окружением; крупные области восстанавливайте несколькими проходами.

**Шов панорамы:** маска ИИ и фрагмент изображения циклически переходят с
правого края на левый, поэтому область около 0°/360° отправляется в модель
как единый фрагмент, без искусственной границы по краям. Отдельная кнопка
**«Сгладить шов 0°/360°»** мягко уравнивает цвет на двух краях вдоль всей
панорамы; ширина перехода определяется радиусом кисти, действие отменяется
через `Ctrl+Z`. Для крупных отличий или объектов на шве используйте ИИ-маску.

**Ограничения:** NAFNet лучше справляется со смазыванием, чем с полностью
потерянной фактурой. LaMa может создавать правдоподобные, но не исторически
точные детали. LaMa заполняет область по видимому окружению и не является
генеративной текстовой моделью. Если выбран очень большой участок у полюса,
применение может потребовать меньшего радиуса.

Модели получены из независимых ONNX-экспортов:
[`NAFNet FP32`](https://github.com/rock1110/nafnet-onnx/blob/main/nafnet-width32-gopro-fp32.onnx),
[`LaMa`](https://huggingface.co/gumka/lama-onnx). При распространении весов
проверяйте их лицензии и атрибуцию; в Git-репозитории веса не хранятся.

## Implemented

- Explore from inside a 360° sphere with mouse drag and mouse wheel zoom.
- Load panorama JPG/PNG/BMP (recommended 2:1).
- Paint directly on the sphere with a geodesic brush, including the 0°/360° seam and poles.
- Non-destructive raster edit layer over the opened base panorama.
- Eraser clears edits and reveals original pixels beneath.
- Clone Stamp copies pixels from another spherical direction with a rotation-aware offset, including across the seam.
- Brush colour, a manual numeric angular-radius field (0.1°–30°), and strength controls; Undo/Redo tracks entire strokes.
- Movable, detachable tool palette with Russian tool labels and menus.
- Save composited result as PNG or JPEG.
- Partial OpenGL texture updates during editing (large panorama performance is still a work in progress).
- Liquify strokes now accumulate from the previous dab without making a full-size
  image copy for each mouse movement; temporary memory depends on brush area.
- Windows build artifacts from GitHub Actions.

## Build (Windows)

Install Visual Studio 2022 with C++ workload, CMake >= 3.24 and Qt 6.x (MSVC 2022 64-bit, OpenGLWidgets module). Then double-click **`build_windows.bat`** in the repository root; it detects Qt from `CMAKE_PREFIX_PATH`, `QTDIR`, or `C:\Qt\6.x\msvc2022_64`, compiles Release and deploys DLLs to **`dist\PanoramaEdit\PanoramaEdit.exe`**.

Alternatively, build manually (replace Qt path as needed):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64"
cmake --build build --config Release
```

For deploying Qt DLLs next to the executable, run Qt's `windeployqt` on `build/Release/PanoramaEdit.exe` (or use the GitHub Actions workflow which handles this).

## Controls

- **Open**: Ctrl+O; **Save As**: Ctrl+Shift+S.
- **Navigate**: `N`; left mouse drag to look around. Right mouse drag to look around in any mode.
- **Brush**: `B`; left mouse drag to paint on the overlay.
- **Eraser**: `E`; left mouse drag to erase paint/clone changes (the original panorama remains).
- **Clone Stamp**: `S`; first **Ctrl + left-click** to select source, then left-drag elsewhere to clone it. Each stroke reads a frozen image snapshot, so painting cannot recursively sample itself.
- **Zoom**: mouse wheel.
- **Undo/Redo**: Ctrl+Z / Ctrl+Y.

The editor currently has one transparent edit layer, not a full Photoshop-style layer stack. Only the composited export is saved: opening the PNG/JPEG again does **not** restore the editable layer. The project format, masks, healing, brush feathering, layer stack and tiled 16K engine are future milestones. A single 16K RGBA panorama uses 512 MiB per full-size image, so avoid excessive strokes/history on memory-constrained PCs.

### Производительность

Видеокарта уже используется для отображения сферы через OpenGL. Инструменты
«Палец», «Деформация», «Вздутие» и «Сжатие» пока рассчитываются процессором,
но больше **не копируют всю панораму при каждом движении мыши**. Теперь
вычисляются только пиксели области кисти, а изменения последовательных мазков
накапливаются. Для максимальной скорости на 8K/16K следующим шагом нужен
отдельный GPU-движок пластики (шейдеры и карта смещений).

## Tests

The Windows GitHub Actions workflow runs CTest after compiling (spherical raster and tool-palette controls). To run the tests locally:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

## Geometry

Pixels remain a standard equirectangular 2:1 image, but brush footprints are calculated by angular distance between spherical directions, not flat screen pixels. Longitude wraps seamlessly at both edges; latitude remains clamped at poles. The Clone Stamp applies the shortest 3D rotation from the dab destination anchor toward the selected sample origin, preserving angular offsets without projecting the brush onto a flat rectangle. The sphere is rendered from its centre with inward-facing texture.
