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

    // A dockable palette can be dragged around the desktop, attached to either
    // side of the viewport, or reopened from the View menu after being closed.
    auto *toolsDock = new QDockWidget(tr("Инструменты"), this);
    toolsDock->setObjectName("ToolsPalette");
    toolsDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    toolsDock->setFeatures(QDockWidget::DockWidgetMovable |
                           QDockWidget::DockWidgetFloatable |
                           QDockWidget::DockWidgetClosable);
    auto *panel = new QWidget(toolsDock);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(10, 10, 10, 10);
    panelLayout->setSpacing(8);
    auto *buttons = new QGridLayout();
    buttons->setHorizontalSpacing(6);
    buttons->setVerticalSpacing(6);
    buttons->setAlignment(Qt::AlignLeft);
    panelLayout->addLayout(buttons);

    const bool darkPalette = panel->palette().color(QPalette::Window).lightness() < 128;
    const QString hoverColor = darkPalette ? "#284c66" : "#d7ecfc";
    const QString checkedColor = darkPalette ? "#1c527a" : "#afdbfa";
    const QString iconButtonStyle = QString(
        "QToolButton { border: 1px solid palette(mid); border-radius: 7px; "
        "background: palette(button); padding: 4px; }"
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
        const QString tooltip = tr("%1 (%2)\n%3").arg(name, shortcut, hint);
        action->setToolTip(tooltip);
        action->setStatusTip(hint);
        action->setShortcut(QKeySequence(shortcut));
        action->setShortcutContext(Qt::ApplicationShortcut);
        group->addAction(action);
        addAction(action);
        auto *button = new QToolButton(panel);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(29, 29));
        button->setFixedSize(48, 46);
        button->setStyleSheet(iconButtonStyle);
        button->setToolTip(tooltip);
        button->setAccessibleName(name);
        button->setAccessibleDescription(hint);
        buttons->addWidget(button, row, column);
        connect(action, &QAction::triggered, this, [this, mode] { view_->setMode(mode); });
        return action;
    };

    navigateAction_ = addTool(tr("Навигация"), tr("Поворачивать сферу"),
                              "N", SphereView::Mode::Navigate, ToolIcons::Kind::Navigate, 0, 0);
    brushAction_ = addTool(tr("Кисть"), tr("Рисовать непосредственно на сфере"),
                           "B", SphereView::Mode::Brush, ToolIcons::Kind::Brush, 0, 1);
    eraserAction_ = addTool(tr("Ластик"), tr("Стереть внесённые изменения"),
                            "E", SphereView::Mode::Eraser, ToolIcons::Kind::Eraser, 0, 2);
    stampAction_ = addTool(tr("Штамп"), tr("Ctrl + щелчок: выбрать образец для копирования"),
                           "S", SphereView::Mode::CloneStamp, ToolIcons::Kind::Stamp, 0, 3);
    navigateAction_->setChecked(true);
    addTool(tr("Палец"), tr("Размазывать пиксели кистью"),
            "F", SphereView::Mode::Smudge, ToolIcons::Kind::Smudge, 0, 4);
    addTool(tr("Деформация"), tr("Тянуть изображение вслед за курсором"),
            "D", SphereView::Mode::Push, ToolIcons::Kind::Push, 1, 0);
    addTool(tr("Вздутие"), tr("Расширить область под кистью"),
            "I", SphereView::Mode::Bloat, ToolIcons::Kind::Bloat, 1, 1);
    addTool(tr("Сжатие"), tr("Сжать область к центру кисти"),
            "C", SphereView::Mode::Pinch, ToolIcons::Kind::Pinch, 1, 2);
    addTool(tr("ИИ-резкость"), tr("Выделите размытую область для улучшения"),
            "U", SphereView::Mode::AiSharpen, ToolIcons::Kind::Sharpen, 1, 3);
    addTool(tr("ИИ-дорисовка"), tr("Выделите участок, который нужно заполнить фоном"),
            "J", SphereView::Mode::AiInpaint, ToolIcons::Kind::Inpaint, 1, 4);

    addTool(tr("Округление"), tr("Щёлкните центр и точки по контуру объекта, затем нажмите кнопку применения"), "R", SphereView::Mode::RoundObject, ToolIcons::Kind::Bloat, 3, 0);
    addTool(tr("Сетка"), tr("Выберите центр и перемещайте внутренние узлы"), "M", SphereView::Mode::MeshWarp, ToolIcons::Kind::Push, 4, 0);
    addTool(tr("Выпрямление"), tr("Отметьте точки вдоль искривлённой линии"), "L", SphereView::Mode::Straighten, ToolIcons::Kind::Push, 4, 1);
    addTool(tr("Умное выделение"), tr("Щелчок по объекту; Shift добавить, Alt исключить"), "K", SphereView::Mode::SmartSelect, ToolIcons::Kind::Sharpen, 4, 2);
    const auto styleActionButton = [buttons](QPushButton *button, ToolIcons::Kind icon,
                                             const QString &name, const QString &hint,
                                             int column) {
        button->setText({});
        button->setIcon(ToolIcons::make(icon));
        button->setIconSize(QSize(29, 29));
        button->setFixedSize(48, 46);
        button->setAccessibleName(name);
        button->setAccessibleDescription(hint);
        button->setToolTip(name + "\n" + hint);
        buttons->addWidget(button, 2, column);
    };

    auto *color = new QPushButton(panel);
    styleActionButton(color, ToolIcons::Kind::Color, tr("Цвет кисти"),
                      tr("Выбрать цвет рисования"), 0);
    connect(color, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(Qt::red, this, tr("Цвет кисти"));
        if (chosen.isValid()) view_->setBrushColor(chosen);
    });
    applyAiButton_ = new QPushButton(panel);
    styleActionButton(applyAiButton_, ToolIcons::Kind::ApplyAi,
                      tr("Применить ИИ к выделению"),
                      tr("Обработать выделенный участок локальной нейросетью"), 1);
    connect(applyAiButton_, &QPushButton::clicked, this, &MainWindow::startAiRestoration);
    auto *clearAi = new QPushButton(panel);
    styleActionButton(clearAi, ToolIcons::Kind::ClearAi,
                      tr("Очистить ИИ-выделение"), tr("Убрать маску выделения"), 2);
    connect(clearAi, &QPushButton::clicked, view_, &SphereView::clearAiSelection);
    auto *seam = new QPushButton(panel);
    styleActionButton(seam, ToolIcons::Kind::Seam, tr("Сгладить шов 0°/360°"),
                      tr("Мягко свести левый и правый края. Ширина — радиус кисти."), 3);
    connect(seam, &QPushButton::clicked, view_, &SphereView::smoothSeam);

    auto *localColor = new QPushButton(tr("Локальная цветокоррекция"), panel);
    panelLayout->addWidget(localColor);
    connect(localColor, &QPushButton::clicked, this, [this]{if(!aiWatcher_)view_->localColorCorrection();});
    auto *objectSelect = new QPushButton(tr("Выбрать объект (ИИ)"), panel);
    auto *objectRemove = new QPushButton(tr("Удалить выбранный объект"), panel);
    panelLayout->addWidget(objectSelect);
    panelLayout->addWidget(objectRemove);
    connect(objectSelect, &QPushButton::clicked, this, [this]{view_->clearAiSelection();view_->setMode(SphereView::Mode::SmartSelect);});
    connect(objectRemove, &QPushButton::clicked, this, [this]{
        if(aiWatcher_)return;
        if(!view_->prepareObjectRemoval()){
            QMessageBox::information(this,tr("Удаление объекта"),tr("Сначала выберите объект и дождитесь появления маски."));return;
        }
        startAiRestoration();
    });
    auto *levelHorizon = new QPushButton(tr("Автовыравнивание горизонта"), panel);
    panelLayout->addWidget(levelHorizon);
    connect(levelHorizon, &QPushButton::clicked, view_, &SphereView::autoLevelHorizon);
    auto *diagnostics = new QPushButton(tr("Диагностика панорамы"), panel);
    panelLayout->addWidget(diagnostics);
    auto *clearDiagnostics = new QPushButton(tr("Скрыть дефекты"), panel);
    panelLayout->addWidget(clearDiagnostics);
    connect(clearDiagnostics, &QPushButton::clicked, view_, &SphereView::clearDiagnostics);
    connect(diagnostics, &QPushButton::clicked, view_, &SphereView::diagnosePanorama);
    auto *smartSeam = new QPushButton(tr("Умный шов"), panel);
    auto *seamView = new QPushButton(tr("Показать шов"), panel);
    auto *seamAi = new QPushButton(tr("ИИ-дорисовка шва"), panel);
    panelLayout->addWidget(smartSeam);
    panelLayout->addWidget(seamView);
    panelLayout->addWidget(seamAi);
    connect(smartSeam, &QPushButton::clicked, view_, &SphereView::previewSeam);
    connect(seamView, &QPushButton::clicked, view_, &SphereView::showSeam);
    connect(seamAi, &QPushButton::clicked, this, [this]{view_->selectSeamForAi();startAiRestoration();});
    auto *roundApply = new QPushButton(panel);
    roundApply->setIcon(ToolIcons::make(ToolIcons::Kind::ApplyAi));
    roundApply->setToolTip(tr("Применить округление объекта"));
    roundApply->setAccessibleName(tr("Применить округление"));
    roundApply->setFixedSize(48,46);
    buttons->addWidget(roundApply,3,1);
    connect(roundApply, &QPushButton::clicked, view_, &SphereView::roundObject);
    auto *poleRow = new QHBoxLayout();
    auto *zenith = new QPushButton(tr("Зенит"), panel);
    auto *nadir = new QPushButton(tr("Надир"), panel);
    auto *horizon = new QPushButton(tr("Обычный вид"), panel);
    poleRow->addWidget(zenith); poleRow->addWidget(nadir); poleRow->addWidget(horizon);
    panelLayout->addLayout(poleRow);
    connect(zenith, &QPushButton::clicked, view_, &SphereView::showZenith);
    connect(nadir, &QPushButton::clicked, view_, &SphereView::showNadir);
    connect(horizon, &QPushButton::clicked, view_, &SphereView::showHorizon);
    auto *poleRepair = new QPushButton(tr("Сгладить полюс"), panel);
    panelLayout->addWidget(poleRepair);
    connect(poleRepair, &QPushButton::clicked, view_, &SphereView::repairPole);
    auto *lineApply = new QPushButton(tr("Выпрямить линию"), panel);
    auto *lineReset = new QPushButton(tr("Сбросить линию"), panel);
    panelLayout->addWidget(lineApply);
    panelLayout->addWidget(lineReset);
    connect(lineApply, &QPushButton::clicked, view_, &SphereView::straightenLine);
    connect(lineReset, &QPushButton::clicked, view_, &SphereView::resetLine);
    auto *meshApply = new QPushButton(tr("Применить сетку"), panel);
    auto *meshReset = new QPushButton(tr("Сбросить сетку"), panel);
    panelLayout->addWidget(meshApply);
    panelLayout->addWidget(meshReset);
    connect(meshApply, &QPushButton::clicked, view_, &SphereView::applyMesh);
    connect(meshReset, &QPushButton::clicked, view_, &SphereView::resetMesh);
    panelLayout->addWidget(new QLabel(tr("Радиус кисти (градусы):"), panel));
    auto *radiusRow = new QHBoxLayout();
    auto *radius = new QSlider(Qt::Horizontal, panel);
    radius->setRange(1, 300);
    radius->setValue(20);
    radius->setToolTip(tr("Ползунок радиуса: от 0,1° до 30°"));
    auto *radiusInput = new QDoubleSpinBox(panel);
    radiusInput->setDecimals(1);
    radiusInput->setRange(0.1, 30.0);
    radiusInput->setSingleStep(0.5);
    radiusInput->setValue(2.0);
    radiusInput->setSuffix(tr(" °"));
    radiusInput->setKeyboardTracking(false);
    radiusInput->setToolTip(tr("Введите радиус вручную и нажмите Enter"));
    radiusInput->setFixedWidth(90);
    radiusRow->addWidget(radius, 1);
    radiusRow->addWidget(radiusInput);
    panelLayout->addLayout(radiusRow);
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

    auto *strengthTitle = new QLabel(tr("Сила воздействия: 50%"), panel);
    panelLayout->addWidget(strengthTitle);
    auto *strength = new QSlider(Qt::Horizontal, panel);
    strength->setRange(1, 100);
    strength->setValue(50);
    strength->setToolTip(tr("Сила инструментов пластики: от 1% до 100%"));
    connect(strength, &QSlider::valueChanged, this, [this, strengthTitle](int v) {
        strengthTitle->setText(tr("Сила воздействия: %1%").arg(v));
        view_->setToolStrength(v / 100.0);
    });
    panelLayout->addWidget(strength);

    toolsDock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, toolsDock);
    toolsDock->setFloating(true);
    auto *viewMenu = menuBar()->addMenu(tr("&Вид"));
    auto *toggleTools = toolsDock->toggleViewAction();
    toggleTools->setText(tr("Панель инструментов"));
    viewMenu->addAction(toggleTools);
    // Set the initial position after the main window has been laid out.
    QTimer::singleShot(0, this, [this, toolsDock] {
        toolsDock->resize(300, toolsDock->sizeHint().height());
        toolsDock->move(mapToGlobal(QPoint(width() - 320, 65)));
    });

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
