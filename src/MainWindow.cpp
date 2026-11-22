#include "MainWindow.h"
#include "SphereView.h"
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QCoreApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>
#include <memory>

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
    panelLayout->addLayout(buttons);

    auto *group = new QActionGroup(this);
    const auto addTool = [this, panel, buttons, group](const QString &name,
            const QString &hint, const QString &shortcut, SphereView::Mode mode,
            int row, int column) -> QAction* {
        auto *action = new QAction(name, this);
        action->setCheckable(true);
        action->setToolTip(hint);
        action->setShortcut(QKeySequence(shortcut));
        action->setShortcutContext(Qt::ApplicationShortcut);
        group->addAction(action);
        addAction(action);
        auto *button = new QToolButton(panel);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setMinimumWidth(120);
        button->setMinimumHeight(32);
        buttons->addWidget(button, row, column);
        connect(action, &QAction::triggered, this, [this, mode] { view_->setMode(mode); });
        return action;
    };

    navigateAction_ = addTool(tr("Навигация"), tr("Поворачивать сферу (N)"),
                              "N", SphereView::Mode::Navigate, 0, 0);
    brushAction_ = addTool(tr("Кисть"), tr("Рисовать на сфере (B)"),
                           "B", SphereView::Mode::Brush, 0, 1);
    eraserAction_ = addTool(tr("Ластик"), tr("Стереть изменения (E)"),
                            "E", SphereView::Mode::Eraser, 1, 0);
    stampAction_ = addTool(tr("Штамп"), tr("Ctrl + щелчок: выбрать образец (S)"),
                           "S", SphereView::Mode::CloneStamp, 1, 1);
    navigateAction_->setChecked(true);
    addTool(tr("Палец"), tr("Размазывать пиксели (F)"),
            "F", SphereView::Mode::Smudge, 2, 0);
    addTool(tr("Деформация"), tr("Тянуть изображение вслед за курсором (D)"),
            "D", SphereView::Mode::Push, 2, 1);
    addTool(tr("Вздутие"), tr("Расширить область под кистью (I)"),
            "I", SphereView::Mode::Bloat, 3, 0);
    addTool(tr("Сжатие"), tr("Сжать область к центру кисти (C)"),
            "C", SphereView::Mode::Pinch, 3, 1);
    addTool(tr("ИИ-резкость"), tr("Выделите размытую область для улучшения (U)"),
            "U", SphereView::Mode::AiSharpen, 4, 0);
    addTool(tr("ИИ-дорисовка"), tr("Выделите участок, который нужно заполнить фоном (J)"),
            "J", SphereView::Mode::AiInpaint, 4, 1);

    auto *color = new QPushButton(tr("Цвет кисти..."), panel);
    color->setMinimumHeight(32);
    buttons->addWidget(color, 5, 0, 1, 2);
    connect(color, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(Qt::red, this, tr("Цвет кисти"));
        if (chosen.isValid()) view_->setBrushColor(chosen);
    });
    applyAiButton_ = new QPushButton(tr("Применить ИИ к выделению"), panel);
    applyAiButton_->setMinimumHeight(34);
    applyAiButton_->setToolTip(tr("Обработать выделенную область локальной нейросетью"));
    buttons->addWidget(applyAiButton_, 6, 0, 1, 2);
    connect(applyAiButton_, &QPushButton::clicked, this, &MainWindow::startAiRestoration);
    auto *clearAi = new QPushButton(tr("Очистить ИИ-выделение"), panel);
    buttons->addWidget(clearAi, 7, 0, 1, 2);
    connect(clearAi, &QPushButton::clicked, view_, &SphereView::clearAiSelection);
    auto *seam = new QPushButton(tr("Сгладить шов 0°/360°"), panel);
    seam->setToolTip(tr("Мягко свести левый и правый края. Ширина — радиус кисти."));
    buttons->addWidget(seam, 8, 0, 1, 2);
    connect(seam, &QPushButton::clicked, view_, &SphereView::smoothSeam);

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
        tr("Изображения (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
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
    if (aiProcess_) return;
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
    const QString python = applicationPath + "/ai_runtime/Scripts/python.exe";
    const QString script = applicationPath + "/ai_worker.py";
    if (!QFileInfo::exists(model) || !QFileInfo::exists(python) || !QFileInfo::exists(script)) {
        QMessageBox::warning(this, tr("ИИ ещё не установлен"),
            tr("Для локального ИИ один раз запустите «setup_ai_windows.bat» "
               "рядом с PanoramaEdit.exe. Он установит среду и две модели. "
               "После этого интернет для обработки панорам не нужен."));
        return;
    }

    auto temporary = std::make_shared<QTemporaryDir>();
    if (!temporary->isValid()) {
        QMessageBox::warning(this, tr("Ошибка ИИ"), tr("Не удалось создать временную папку."));
        return;
    }
    const QString input = temporary->filePath("input.png");
    const QString mask = temporary->filePath("mask.png");
    const QString output = temporary->filePath("output.png");
    if (!selected->image.save(input) || !selected->mask.save(mask)) {
        QMessageBox::warning(this, tr("Ошибка ИИ"), tr("Не удалось сохранить выделенную область."));
        return;
    }

    auto *process = new QProcess(this);
    aiProcess_ = process;
    applyAiButton_->setEnabled(false);
    const quint64 documentId = view_->documentId();
    auto *progress = new QProgressDialog(tr("Нейросеть обрабатывает выделение офлайн..."),
        tr("Отмена"), 0, 0, this);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setWindowModality(Qt::ApplicationModal);
    connect(progress, &QProgressDialog::canceled, process, &QProcess::kill);

    connect(process, &QProcess::finished, this,
        [this, process, progress, temporary, selected = std::move(*selected),
         output, documentId](int code, QProcess::ExitStatus status) {
        if (aiProcess_ != process) return;
        const bool canceled = progress->wasCanceled();
        progress->close();
        progress->deleteLater();
        aiProcess_ = nullptr;
        applyAiButton_->setEnabled(true);
        if (canceled) {
            statusBar()->showMessage(tr("ИИ-обработка отменена"), 5000);
        } else if (status != QProcess::NormalExit || code != 0) {
            QMessageBox::warning(this, tr("Ошибка ИИ-модели"),
                QString::fromUtf8(process->readAll()).left(1500));
        } else if (documentId != view_->documentId()) {
            statusBar()->showMessage(tr("Панорама сменилась: старый результат ИИ отброшен"), 6000);
        } else {
            QImage result(output);
            if (result.isNull() || !view_->applyAiResult(selected, result)) {
                QMessageBox::information(this, tr("ИИ-восстановление"),
                    tr("Модель не вернула изменений в выбранной области."));
            } else {
                statusBar()->showMessage(tr("ИИ-восстановление применено. Ctrl+Z — отменить."), 7000);
            }
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this,
        [this, process, progress](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || aiProcess_ != process) return;
        progress->close();
        progress->deleteLater();
        aiProcess_ = nullptr;
        applyAiButton_->setEnabled(true);
        QMessageBox::warning(this, tr("Не удалось запустить ИИ"), process->errorString());
        process->deleteLater();
    });
    process->setProgram(python);
    process->setArguments({script, "--mode", mode == SphereView::Mode::AiSharpen
        ? "sharpen" : "inpaint", "--model", model, "--image", input,
        "--mask", mask, "--output", output});
    process->setWorkingDirectory(applicationPath);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->start();
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
    if (confirmDiscard()) event->accept();
    else event->ignore();
}
