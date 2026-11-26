#include "MainWindow.h"
#include "SphereView.h"
#include "AiEngine.h"
#include "ToolIcons.h"
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QCoreApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QGroupBox>
#include <QFormLayout>
#include <functional>
#include <QMessageBox>
#include <QPalette>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <atomic>
#include <memory>
#include <algorithm>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    view_ = new SphereView(this);
    setCentralWidget(view_);
    setWindowTitle(tr("PanoramaEdit — редактор панорам 360°"));
    resize(1280, 800);
    auto *file = menuBar()->addMenu(tr("&Файл"));
    auto *open = file->addAction(tr("&Открыть панораму..."));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::openFile);
    auto *save = file->addAction(tr("Сохранить &как..."));
    save->setShortcut(QKeySequence("Ctrl+Shift+S"));
    connect(save, &QAction::triggered, this, [this] { saveFile(); });
    file->addSeparator();
    file->addAction(tr("Выход"), this, &QWidget::close);

    auto *edit = menuBar()->addMenu(tr("&Правка"));
    auto *undo = edit->addAction(tr("&Отменить"));
    undo->setShortcut(QKeySequence::Undo);
    connect(undo, &QAction::triggered, view_, &SphereView::undo);
    auto *redo = edit->addAction(tr("&Повторить"));
    redo->setShortcut(QKeySequence("Ctrl+Y"));
    connect(redo, &QAction::triggered, view_, &SphereView::redo);

    // Compact primary-tool palette. Secondary and occasional commands live in menus.
    auto *toolsDock = new QDockWidget(tr("Инструменты"), this);
    toolsDock->setObjectName("ToolsPalette");
    toolsDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    toolsDock->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);

    auto *panel = new QWidget(toolsDock);
    panel->setMinimumWidth(226);
    panel->setMaximumWidth(270);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(6, 6, 6, 6);
    panelLayout->setSpacing(5);

    auto *buttons = new QGridLayout();
    buttons->setHorizontalSpacing(4);
    buttons->setVerticalSpacing(4);
    buttons->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    panelLayout->addLayout(buttons);

    const bool darkPalette = panel->palette().color(QPalette::Window).lightness() < 128;
    const QString hoverColor = darkPalette ? "#284c66" : "#d7ecfc";
    const QString checkedColor = darkPalette ? "#1c527a" : "#afdbfa";
    const QString iconButtonStyle = QString(
        "QToolButton { border: 1px solid palette(mid); border-radius: 6px; "
        "background: palette(button); padding: 2px; }"
        "QToolButton:hover { background: %1; border-color: #348ec5; }"
        "QToolButton:checked { background: %2; border: 2px solid #167db8; }"
        "QToolButton:pressed { background: %1; }").arg(hoverColor, checkedColor);

    auto *group = new QActionGroup(this);
    const auto addTool = [this, panel, buttons, group, &iconButtonStyle](const QString &name,
            const QString &hint, const QString &shortcut, SphereView::Mode mode,
            ToolIcons::Kind icon, int row, int column) -> QAction* {
        auto *action = new QAction(name, this);
        action->setCheckable(true);
        action->setIcon(ToolIcons::make(icon));
        const QString tooltip = shortcut.isEmpty()
            ? name + "\\n" + hint : tr("%1 (%2)\\n%3").arg(name, shortcut, hint);
        action->setToolTip(tooltip);
        action->setStatusTip(hint);
        if (!shortcut.isEmpty()) action->setShortcut(QKeySequence(shortcut));
        action->setShortcutContext(Qt::ApplicationShortcut);
        group->addAction(action);
        addAction(action);
        auto *button = new QToolButton(panel);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(24, 24));
        button->setFixedSize(39, 38);
        button->setStyleSheet(iconButtonStyle);
        button->setToolTip(tooltip);
        button->setAccessibleName(name);
        button->setAccessibleDescription(hint);
        buttons->addWidget(button, row, column);
        connect(action, &QAction::triggered, this, [this, mode] { view_->setMode(mode); });
        return action;
    };

    navigateAction_ = addTool(tr("Навигация"), tr("Поворачивать сферу"), "N",
        SphereView::Mode::Navigate, ToolIcons::Kind::Navigate, 0, 0);
    brushAction_ = addTool(tr("Кисть"), tr("Рисовать непосредственно на сфере"), "B",
        SphereView::Mode::Brush, ToolIcons::Kind::Brush, 0, 1);
    eraserAction_ = addTool(tr("Ластик"), tr("Стереть внесённые изменения"), "E",
        SphereView::Mode::Eraser, ToolIcons::Kind::Eraser, 0, 2);
    stampAction_ = addTool(tr("Штамп"), tr("Ctrl + щелчок: выбрать образец для копирования"), "S",
        SphereView::Mode::CloneStamp, ToolIcons::Kind::Stamp, 0, 3);
    navigateAction_->setChecked(true);
    addTool(tr("Палец"), tr("Размазывать пиксели кистью"), "F",
        SphereView::Mode::Smudge, ToolIcons::Kind::Smudge, 0, 4);
    addTool(tr("Деформация"), tr("Тянуть изображение вслед за курсором"), "D",
        SphereView::Mode::Push, ToolIcons::Kind::Push, 1, 0);
    addTool(tr("Вздутие"), tr("Расширить область под кистью"), "I",
        SphereView::Mode::Bloat, ToolIcons::Kind::Bloat, 1, 1);
    addTool(tr("Сжатие"), tr("Сжать область к центру кисти"), "C",
        SphereView::Mode::Pinch, ToolIcons::Kind::Pinch, 1, 2);
    addTool(tr("ИИ-резкость"), tr("Выделите размытую область для улучшения"), "U",
        SphereView::Mode::AiSharpen, ToolIcons::Kind::Sharpen, 1, 3);
    addTool(tr("ИИ-дорисовка"), tr("Выделите участок для восстановления фона"), "J",
        SphereView::Mode::AiInpaint, ToolIcons::Kind::Inpaint, 1, 4);
    addTool(tr("Округление"), tr("Выберите центр и точки контура объекта"), "R",
        SphereView::Mode::RoundObject, ToolIcons::Kind::Bloat, 2, 0);
    addTool(tr("Сетка"), tr("Выберите центр и перемещайте узлы сетки"), "M",
        SphereView::Mode::MeshWarp, ToolIcons::Kind::Push, 2, 1);
    addTool(tr("Выпрямление"), tr("Отметьте точки вдоль искривлённой линии"), "L",
        SphereView::Mode::Straighten, ToolIcons::Kind::Push, 2, 2);
    addTool(tr("Умное выделение"), tr("Щелчок по объекту; Shift добавить, Alt исключить"), "K",
        SphereView::Mode::SmartSelect, ToolIcons::Kind::Sharpen, 2, 3);
    addTool(tr("Тянуть"), tr("Зажмите участок и переместите его по поверхности сферы"), "W",
        SphereView::Mode::SphericalWarp, ToolIcons::Kind::Push, 3, 4);
    addTool(tr("Растягивать"), tr("Вытягивать участок вдоль движения кисти, плавно сохраняя соседние области"), "T",
        SphereView::Mode::SphericalStretch, ToolIcons::Kind::Bloat, 3, 5);
    addTool(tr("Сжимать"), tr("Собирать участок вдоль движения кисти, не сдвигая всю сферу"), "G",
        SphereView::Mode::SphericalCompress, ToolIcons::Kind::Pinch, 3, 6);

    const auto addIconButton = [buttons, panel](QPushButton *button, ToolIcons::Kind icon,
            const QString &name, const QString &hint, int row, int column) {
        button->setText({});
        button->setIcon(ToolIcons::make(icon));
        button->setIconSize(QSize(23, 23));
        button->setFixedSize(39, 38);
        button->setAccessibleName(name);
        button->setAccessibleDescription(hint);
        button->setToolTip(name + "\\n" + hint);
        buttons->addWidget(button, row, column);
    };

    auto *color = new QPushButton(panel);
    addIconButton(color, ToolIcons::Kind::Color, tr("Цвет кисти"),
                  tr("Выбрать цвет рисования"), 3, 0);
    connect(color, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(Qt::red, this, tr("Цвет кисти"));
        if (chosen.isValid()) view_->setBrushColor(chosen);
    });
    applyAiButton_ = new QPushButton(panel);
    addIconButton(applyAiButton_, ToolIcons::Kind::ApplyAi, tr("Применить ИИ"),
                  tr("Обработать выделенную область локальной нейросетью"), 3, 1);
    connect(applyAiButton_, &QPushButton::clicked, this, &MainWindow::startAiRestoration);
    auto *clearAi = new QPushButton(panel);
    addIconButton(clearAi, ToolIcons::Kind::ClearAi, tr("Снять выделение"),
                  tr("Убрать маску ИИ-выделения"), 3, 2);
    connect(clearAi, &QPushButton::clicked, view_, &SphereView::clearAiSelection);
    auto *seam = new QPushButton(panel);
    addIconButton(seam, ToolIcons::Kind::Seam, tr("Сгладить шов"),
                  tr("Мягко свести левый и правый края"), 3, 3);
    connect(seam, &QPushButton::clicked, view_, &SphereView::smoothSeam);

    auto *settingsGroup = new QGroupBox(tr("Кисть и воздействие"), panel);
    auto *settingsLayout = new QGridLayout(settingsGroup);
    settingsLayout->setContentsMargins(7, 7, 7, 7);
    settingsLayout->setHorizontalSpacing(5);
    settingsLayout->setVerticalSpacing(3);
    auto *radiusLabel = new QLabel(tr("Радиус"), settingsGroup);
    auto *radius = new QSlider(Qt::Horizontal, settingsGroup);
    radius->setRange(1, 300);
    radius->setValue(20);
    radius->setToolTip(tr("Радиус кисти от 0,1° до 30°"));
    auto *radiusInput = new QDoubleSpinBox(settingsGroup);
    radiusInput->setDecimals(1);
    radiusInput->setRange(0.1, 30.0);
    radiusInput->setSingleStep(0.5);
    radiusInput->setValue(2.0);
    radiusInput->setSuffix(tr("°"));
    radiusInput->setKeyboardTracking(false);
    radiusInput->setFixedWidth(66);
    settingsLayout->addWidget(radiusLabel, 0, 0);
    settingsLayout->addWidget(radius, 0, 1);
    settingsLayout->addWidget(radiusInput, 0, 2);
    connect(radius, &QSlider::valueChanged, this, [this, radiusInput](int v) {
        const QSignalBlocker block(radiusInput);
        radiusInput->setValue(v / 10.0);
        view_->setBrushRadius(v / 10.0);
    });
    connect(radiusInput, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this, radius](double v) {
        const QSignalBlocker block(radius);
        radius->setValue(qRound(v * 10.0));
        view_->setBrushRadius(v);
    });

    auto *strengthLabel = new QLabel(tr("Сила"), settingsGroup);
    auto *strength = new QSlider(Qt::Horizontal, settingsGroup);
    strength->setRange(1, 100);
    strength->setValue(50);
    strength->setToolTip(tr("Сила воздействия инструментов пластики"));
    auto *strengthValue = new QLabel(tr("50%"), settingsGroup);
    strengthValue->setMinimumWidth(34);
    settingsLayout->addWidget(strengthLabel, 1, 0);
    settingsLayout->addWidget(strength, 1, 1);
    settingsLayout->addWidget(strengthValue, 1, 2);
    connect(strength, &QSlider::valueChanged, this, [this, strengthValue](int v) {
        strengthValue->setText(tr("%1%").arg(v));
        view_->setToolStrength(v / 100.0);
    });
    panelLayout->addWidget(settingsGroup);
    panelLayout->addStretch(1);

    toolsDock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, toolsDock);
    toolsDock->setMinimumWidth(242);
    toolsDock->setMaximumWidth(286);

    auto *viewMenu = menuBar()->addMenu(tr("&Вид"));
    auto *toggleTools = toolsDock->toggleViewAction();
    toggleTools->setText(tr("Панель инструментов"));
    viewMenu->addAction(toggleTools);

    const auto addMenuAction = [this](QMenu *menu, const QString &name,
            const QString &hint, const std::function<void()> &callback,
            ToolIcons::Kind icon = ToolIcons::Kind::Navigate) {
        auto *action = menu->addAction(ToolIcons::make(icon), name);
        action->setStatusTip(hint);
        action->setToolTip(hint);
        connect(action, &QAction::triggered, this, callback);
        return action;
    };

    auto *correctionMenu = menuBar()->addMenu(tr("&Коррекция"));
    addMenuAction(correctionMenu, tr("HDR и локальный контраст"),
        tr("Восстановление тонов и усиление локальных деталей"), [this] {
            if (!aiWatcher_) view_->colorGrading();
        }, ToolIcons::Kind::Color);
    addMenuAction(correctionMenu, tr("Dehaze + Clarity + Texture"),
        tr("Удаление дымки, локальный контраст и детализация фактуры"), [this] {
            if (!aiWatcher_) view_->colorGrading();
        }, ToolIcons::Kind::Color);
    addMenuAction(correctionMenu, tr("Цветокоррекция и уровни"),
        tr("Цветовые каналы, яркость, контраст и тон"), [this] {
            if (!aiWatcher_) view_->colorGrading();
        }, ToolIcons::Kind::Color);
    addMenuAction(correctionMenu, tr("Локальная цветокоррекция"),
        tr("Коррекция выбранного объекта или области"), [this] {
            if (!aiWatcher_) view_->localColorCorrection();
        }, ToolIcons::Kind::Color);
    addMenuAction(correctionMenu, tr("Автокоррекция панорамы"),
        tr("Баланс освещения, цвета и шва"), [this] {
            if (!aiWatcher_) view_->panoramaAutoCorrection();
        }, ToolIcons::Kind::Color);
    addMenuAction(correctionMenu, tr("Сферический градиент"),
        tr("Плавная коррекция по сфере"), [this] {
            if (!aiWatcher_) view_->sphericalGradient();
        }, ToolIcons::Kind::Color);
    correctionMenu->addSeparator();
    addMenuAction(correctionMenu, tr("Диагностика панорамы"),
        tr("Найти дефекты изображения"), [this] { view_->diagnosePanorama(); });
    addMenuAction(correctionMenu, tr("Скрыть диагностику"),
        tr("Убрать отображение найденных дефектов"), [this] { view_->clearDiagnostics(); });

    auto *sphereMenu = menuBar()->addMenu(tr("&Сфера 360°"));
    addMenuAction(sphereMenu, tr("Натяжение текстуры от полюса"),
        tr("Переместить текстуру от зенита или надира"), [this] {
            if (!aiWatcher_) view_->stretchPoleTexture();
        });
    addMenuAction(sphereMenu, tr("Ручное выравнивание сферы"),
        tr("Сместить текстуру по сфере без вращения камеры"), [this] {
            if (!aiWatcher_) view_->manualLevelHorizon();
        });
    addMenuAction(sphereMenu, tr("Автовыравнивание горизонта"),
        tr("Автоматически определить и исправить наклон"), [this] {
            view_->autoLevelHorizon();
        });
    sphereMenu->addSeparator();
    addMenuAction(sphereMenu, tr("Показать зенит"), tr("Посмотреть верхнюю точку сферы"),
        [this] { view_->showZenith(); });
    addMenuAction(sphereMenu, tr("Показать надир"), tr("Посмотреть нижнюю точку сферы"),
        [this] { view_->showNadir(); });
    addMenuAction(sphereMenu, tr("Обычный вид"), tr("Вернуться к обычному обзору"),
        [this] { view_->showHorizon(); });
    addMenuAction(sphereMenu, tr("Сгладить полюс"), tr("Исправить растяжение текстуры у полюса"),
        [this] { view_->repairPole(); });

    auto *selectionMenu = menuBar()->addMenu(tr("&Выделение и объекты"));
    addMenuAction(selectionMenu, tr("Выбрать объект (ИИ)"),
        tr("Перейти к умному выделению объекта"), [this] {
            view_->clearAiSelection();
            view_->setMode(SphereView::Mode::SmartSelect);
        }, ToolIcons::Kind::Sharpen);
    addMenuAction(selectionMenu, tr("Удалить выбранный объект"),
        tr("Восстановить фон на месте выбранного объекта"), [this] {
            if (aiWatcher_) return;
            if (!view_->prepareObjectRemoval()) {
                QMessageBox::information(this, tr("Удаление объекта"),
                    tr("Сначала выберите объект и дождитесь появления маски."));
                return;
            }
            startAiRestoration();
        }, ToolIcons::Kind::Inpaint);
    addMenuAction(selectionMenu, tr("Округление по выделению"),
        tr("Построить округлый объект по маске выделения"), [this] {
            view_->roundFromSelection();
        }, ToolIcons::Kind::Bloat);
    addMenuAction(selectionMenu, tr("Применить округление"),
        tr("Применить рассчитанную форму объекта"), [this] {
            view_->roundObject();
        }, ToolIcons::Kind::ApplyAi);
    addMenuAction(selectionMenu, tr("Очистить ИИ-выделение"),
        tr("Убрать маску выделения"), [this] {
            view_->clearAiSelection();
        }, ToolIcons::Kind::ClearAi);

    auto *geometryMenu = menuBar()->addMenu(tr("&Геометрия"));
    addMenuAction(geometryMenu, tr("Архитектурная коррекция — вертикали"),
        tr("Выберите две точки на вертикальном ребре здания и выровняйте его"), [this] {
            view_->architecturalCorrection();
        }, ToolIcons::Kind::Push);
    geometryMenu->addSeparator();
    addMenuAction(geometryMenu, tr("Выпрямить линию"),
        tr("Выпрямить отмеченную линию"), [this] { view_->straightenLine(); });
    addMenuAction(geometryMenu, tr("Сбросить линию"),
        tr("Отменить построение линии"), [this] { view_->resetLine(); });
    geometryMenu->addSeparator();
    addMenuAction(geometryMenu, tr("Применить сетку"),
        tr("Применить деформацию по сетке"), [this] { view_->applyMesh(); });
    addMenuAction(geometryMenu, tr("Сбросить сетку"),
        tr("Отменить деформацию по сетке"), [this] { view_->resetMesh(); });
    geometryMenu->addSeparator();
    addMenuAction(geometryMenu, tr("Сгладить шов 0°/360°"),
        tr("Свести левую и правую границы панорамы"), [this] { view_->smoothSeam(); },
        ToolIcons::Kind::Seam);
    addMenuAction(geometryMenu, tr("Умный шов — предпросмотр"),
        tr("Проанализировать несовпадение краёв"), [this] { view_->previewSeam(); });
    addMenuAction(geometryMenu, tr("Показать шов"),
        tr("Показать область соединения краёв"), [this] { view_->showSeam(); });
    addMenuAction(geometryMenu, tr("ИИ-дорисовка шва"),
        tr("Выделить шов и восстановить его нейросетью"), [this] {
            view_->selectSeamForAi();
            startAiRestoration();
        }, ToolIcons::Kind::Inpaint);

    // Keep the palette docked by default; users can float or move it from the title bar.
    statusLabel_ = new QLabel(tr("Откройте панораму 360° для редактирования"), this);
    statusBar()->addPermanentWidget(statusLabel_);
    connect(view_, &SphereView::imageChanged, this, [this] {
        statusLabel_->setText(tr("Панорама 360° × 180°  |  Эквиректангл"));
    });
    connect(view_, &SphereView::statusMessage, this, [this](const QString &message) {
        statusBar()->showMessage(message, 6000);
    });
    connect(view_, &SphereView::modifiedChanged, this, [this](bool modified) {
        setWindowModified(modified);
        setWindowTitle(QString("PanoramaEdit — %1[*]").arg(filePath_.isEmpty() ? tr("Без названия") : QFileInfo(filePath_).fileName()));
    });
}

bool MainWindow::confirmDiscard() {
    if (!view_->isModified()) return true;
    QMessageBox dialog(QMessageBox::Warning, tr("Несохранённые изменения"),
        tr("Закрыть панораму без сохранения изменений?"), QMessageBox::NoButton, this);
    auto *discard = dialog.addButton(tr("Не сохранять"), QMessageBox::DestructiveRole);
    dialog.addButton(tr("Отмена"), QMessageBox::RejectRole);
    dialog.exec();
    return dialog.clickedButton() == discard;
}

void MainWindow::openFile() {
    if (!confirmDiscard()) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Открыть панораму"), {},
        tr("Изображения (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"));
    if (path.isEmpty()) return;
    QString error;
    if (!view_->openImage(path, &error)) {
        QMessageBox::critical(this, tr("Не удалось открыть панораму"), error);
        return;
    }
    filePath_ = path;
    setWindowTitle(QString("PanoramaEdit — %1[*]").arg(QFileInfo(filePath_).fileName()));
}

void MainWindow::startAiRestoration() {
    if (aiWatcher_) return;
    const SphereView::Mode mode = view_->mode();
    if (mode != SphereView::Mode::AiSharpen && mode != SphereView::Mode::AiInpaint) {
        QMessageBox::information(this, tr("ИИ-восстановление"),
            tr("Сначала выберите «ИИ-резкость» или «ИИ-дорисовка», "
               "затем закрасьте нужную область на сфере."));
        return;
    }
    auto selected = view_->prepareAiPatch();
    if (!selected) {
        QMessageBox::information(this, tr("ИИ-восстановление"),
            tr("Нет выделения или оно слишком большое. Выделите кистью "
               "один небольшой участок (до 1024 пикселей с окружением)."));
        return;
    }

    const QString applicationPath = QCoreApplication::applicationDirPath();
    const QString model = applicationPath + (mode == SphereView::Mode::AiSharpen
        ? "/models/deblur.onnx" : "/models/inpaint.onnx");
    if (!QFileInfo::exists(model)) {
        QMessageBox::warning(this, tr("ИИ ещё не установлен"),
            tr("Для локального ИИ один раз запустите «setup_ai_windows.bat» "
               "рядом с PanoramaEdit.exe. Он скачает модели без установки Python. "
               "После этого интернет для обработки панорам не нужен."));
        return;
    }
    auto *watcher = new QFutureWatcher<AiEngine::Result>(this);
    aiWatcher_ = watcher;
    applyAiButton_->setEnabled(false);
    const quint64 documentId = view_->documentId();
    const auto aiMode = mode == SphereView::Mode::AiSharpen
        ? AiEngine::Mode::Sharpen : AiEngine::Mode::Inpaint;
    const QString modeName = aiMode == AiEngine::Mode::Sharpen
        ? tr("ИИ-резкость") : tr("ИИ-дорисовка");
    const QString statsKey = aiMode == AiEngine::Mode::Sharpen
        ? QStringLiteral("ai/sharpen/") : QStringLiteral("ai/inpaint/");
    QSettings settings;
    const double previousGpuRate = settings.value(statsKey + "gpu_ms_per_pixel").toDouble();
    const double previousCpuRate = settings.value(statsKey + "cpu_ms_per_pixel").toDouble();
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    auto processingStart = std::make_shared<qint64>(-1);
    auto progressState = std::make_shared<AiEngine::Progress>();

    auto *progress = new QProgressDialog(tr("Загрузка локальной ИИ-модели..."),
        tr("Отмена"), 0, 0, this);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setWindowTitle(tr("Обработка нейросетью"));
    progress->setMinimumWidth(390);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    connect(progress, &QProgressDialog::canceled, watcher,
            [this, canceled] {
                canceled->store(true, std::memory_order_relaxed);
                statusBar()->showMessage(tr("Отмена ИИ: ожидаем завершения текущего блока..."), 8000);
            });
    // Preserve the image data before moving the patch to the completion callback.
    const QImage aiImage = selected->image;
    const QImage aiMask = selected->mask;
    const double pixels = double(aiImage.width()) * aiImage.height();

    auto *progressTimer = new QTimer(progress);
    progressTimer->setInterval(250);
    connect(progressTimer, &QTimer::timeout, progress,
        [progress, progressState, canceled, elapsed, processingStart,
         modeName, previousGpuRate, previousCpuRate, pixels] {
        if (canceled->load(std::memory_order_relaxed)) return;
        const auto phase = progressState->phase.load(std::memory_order_acquire);
        const int total = progressState->total.load(std::memory_order_acquire);
        const int completed = progressState->completed.load(std::memory_order_acquire);
        const bool gpu = progressState->usingGpu.load(std::memory_order_acquire);
        const QString processor = gpu ? QStringLiteral("GPU / DirectML") : QStringLiteral("CPU");
        const qint64 elapsedMs = elapsed->elapsed();
        const auto duration = [](qint64 ms) {
            const qint64 seconds = std::max<qint64>(0, ms / 1000);
            return QStringLiteral("%1:%2").arg(seconds / 60)
                .arg(seconds % 60, 2, 10, QLatin1Char('0'));
        };
        if (phase == AiEngine::Phase::Loading) {
            *processingStart = -1;
            progress->setRange(0, 0);
            progress->setLabelText(modeName + QStringLiteral(" — загрузка модели...\nПрошло: ")
                                   + duration(elapsedMs));
            return;
        }
        if (*processingStart < 0) *processingStart = elapsedMs;
        const qint64 processingMs = std::max<qint64>(0, elapsedMs - *processingStart);
        if (phase == AiEngine::Phase::Blending || phase == AiEngine::Phase::Finished) {
            progress->setRange(0, 100);
            progress->setValue(phase == AiEngine::Phase::Finished ? 100 : 99);
            progress->setLabelText(modeName + QStringLiteral(" — ") + processor
                + QStringLiteral("\nСборка результата...\nПрошло: ") + duration(elapsedMs));
            return;
        }
        if (total <= 0) return;
        const double historicalRate = gpu ? previousGpuRate : previousCpuRate;
        int percent = 0;
        QString details;
        QString remaining;
        if (total > 1) {
            percent = std::min(99, completed * 100 / total);
            details = QStringLiteral("Готово %1 из %2 блоков — %3%")
                .arg(completed).arg(total).arg(percent);
            if (completed > 0 && completed < total)
                remaining = QStringLiteral(" • осталось ≈ %1")
                    .arg(duration(processingMs * (total - completed) / completed));
        } else if (historicalRate > 0.0) {
            const qint64 estimatedMs = qint64(historicalRate * pixels);
            percent = estimatedMs > 0
                ? int(std::clamp(100.0 * double(processingMs) / estimatedMs, 0.0, 95.0)) : 0;
            details = QStringLiteral("≈ %1% (оценка по прошлому запуску)").arg(percent);
            if (estimatedMs > processingMs)
                remaining = QStringLiteral(" • осталось ≈ %1")
                    .arg(duration(estimatedMs - processingMs));
        } else {
            details = QStringLiteral("1 блок — точный процент пока недоступен");
        }
        progress->setRange(0, 100);
        progress->setValue(percent);
        progress->setLabelText(modeName + QStringLiteral(" — ") + processor + "\n"
            + details + QStringLiteral("\nПрошло: ") + duration(elapsedMs) + remaining);
    });
    progressTimer->start();

    connect(watcher, &QFutureWatcher<AiEngine::Result>::finished, this,
        [this, watcher, progress, selected = std::move(*selected),
         canceled, documentId, progressTimer, elapsed, statsKey, pixels]() mutable {
        if (aiWatcher_ != watcher) return;
        progressTimer->stop();
        const bool wasCanceled = canceled->load(std::memory_order_relaxed);
        progress->close();
        progress->deleteLater();
        aiWatcher_ = nullptr;
        applyAiButton_->setEnabled(true);
        const AiEngine::Result result = watcher->result();
        if (!wasCanceled && result.error.isEmpty() && !result.image.isNull() && pixels > 0) {
            QSettings settings;
            const QString key = statsKey + (result.usedGpu ? "gpu_ms_per_pixel" : "cpu_ms_per_pixel");
            const double observedRate = double(elapsed->elapsed()) / pixels;
            const double previousRate = settings.value(key).toDouble();
            settings.setValue(key, previousRate > 0.0
                ? previousRate * 0.65 + observedRate * 0.35 : observedRate);
        }
        if (wasCanceled) {
            statusBar()->showMessage(tr("ИИ-обработка отменена"), 5000);
        } else if (!result.error.isEmpty()) {
            QMessageBox::warning(this, tr("Ошибка ИИ-модели"), result.error.left(2000));
        } else if (documentId != view_->documentId()) {
            statusBar()->showMessage(tr("Панорама сменилась: старый результат ИИ отброшен"), 6000);
        } else {
            if (result.image.isNull() || !view_->applyAiResult(selected, result.image)) {
                QMessageBox::information(this, tr("ИИ-восстановление"),
                    tr("Модель не вернула изменений в выбранной области."));
            } else {
                statusBar()->showMessage(result.usedGpu
                    ? tr("ИИ-восстановление через видеокарту. Ctrl+Z — отменить.")
                    : tr("ИИ-восстановление на процессоре. Ctrl+Z — отменить."), 7000);
            }
        }
        watcher->deleteLater();
    });
    auto future = QtConcurrent::run([aiMode, model, aiImage, aiMask, canceled, progressState] {
        return AiEngine::run(aiMode, model, aiImage, aiMask, canceled, progressState);
    });
    watcher->setFuture(future);
    progress->show();
}

bool MainWindow::saveFile() {
    if (!view_->hasImage()) return false;
    QString proposed = filePath_.isEmpty() ? QString("panorama-edited.png") : QFileInfo(filePath_).completeBaseName() + "-edited.png";
    const QString path = QFileDialog::getSaveFileName(this, tr("Сохранить панораму как"), proposed,
        tr("PNG (*.png);;JPEG (*.jpg *.jpeg)"));
    if (path.isEmpty()) return false;
    if (!view_->saveImage(path)) {
        QMessageBox::critical(this, tr("Ошибка сохранения"),
            tr("Не удалось сохранить изображение."));
        return false;
    }
    filePath_ = path;
    setWindowTitle(QString("PanoramaEdit — %1[*]").arg(QFileInfo(filePath_).fileName()));
    return true;
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (aiWatcher_) {
        QMessageBox::information(this, tr("ИИ-обработка"),
            tr("Дождитесь завершения ИИ-обработки перед закрытием редактора."));
        event->ignore();
        return;
    }
    if (confirmDiscard()) event->accept();
    else event->ignore();
}
