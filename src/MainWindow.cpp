#include "MainWindow.h"
#include "SphereView.h"
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

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

    auto *color = new QPushButton(tr("Цвет кисти..."), panel);
    color->setMinimumHeight(32);
    buttons->addWidget(color, 4, 0, 1, 2);
    connect(color, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(Qt::red, this, tr("Цвет кисти"));
        if (chosen.isValid()) view_->setBrushColor(chosen);
    });

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
