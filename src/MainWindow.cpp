#include "MainWindow.h"
#include "SphereView.h"
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QColorDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSlider>
#include <QStatusBar>
#include <QToolBar>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    view_ = new SphereView(this);
    setCentralWidget(view_);
    setWindowTitle("PanoramaEdit — 360° Sphere Editor");
    resize(1280, 800);
    auto *file = menuBar()->addMenu(tr("&File"));
    auto *open = file->addAction(tr("&Open panorama..."));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::openFile);
    auto *save = file->addAction(tr("Save &As..."));
    save->setShortcut(QKeySequence("Ctrl+Shift+S"));
    connect(save, &QAction::triggered, this, [this] { saveFile(); });
    file->addSeparator();
    file->addAction(tr("Exit"), this, &QWidget::close);

    auto *edit = menuBar()->addMenu(tr("&Edit"));
    auto *undo = edit->addAction(tr("&Undo"));
    undo->setShortcut(QKeySequence::Undo);
    connect(undo, &QAction::triggered, view_, &SphereView::undo);
    auto *redo = edit->addAction(tr("&Redo"));
    redo->setShortcut(QKeySequence("Ctrl+Y"));
    connect(redo, &QAction::triggered, view_, &SphereView::redo);

    auto *toolbar = addToolBar(tr("Tools"));
    toolbar->setMovable(false);
    auto *group = new QActionGroup(this);
    navigateAction_ = toolbar->addAction(tr("Navigate"));
    brushAction_ = toolbar->addAction(tr("Brush"));
    eraserAction_ = toolbar->addAction(tr("Eraser"));
    stampAction_ = toolbar->addAction(tr("Clone Stamp"));
    navigateAction_->setShortcut(QKeySequence("N"));
    brushAction_->setShortcut(QKeySequence("B"));
    eraserAction_->setShortcut(QKeySequence("E"));
    stampAction_->setShortcut(QKeySequence("S"));
    for (auto *action : {navigateAction_, brushAction_, eraserAction_, stampAction_}) {
        action->setCheckable(true);
        group->addAction(action);
    }
    navigateAction_->setChecked(true);
    connect(navigateAction_, &QAction::triggered, this, [this] { view_->setMode(SphereView::Mode::Navigate); });
    connect(brushAction_, &QAction::triggered, this, [this] { view_->setMode(SphereView::Mode::Brush); });
    connect(eraserAction_, &QAction::triggered, this, [this] { view_->setMode(SphereView::Mode::Eraser); });
    connect(stampAction_, &QAction::triggered, this, [this] { view_->setMode(SphereView::Mode::CloneStamp); });
    toolbar->addSeparator();
    auto *color = toolbar->addAction(tr("Brush color..."));
    connect(color, &QAction::triggered, this, [this] {
        const QColor chosen = QColorDialog::getColor(Qt::red, this, tr("Brush color"));
        if (chosen.isValid()) view_->setBrushColor(chosen);
    });
    toolbar->addWidget(new QLabel(tr("  Radius (degrees): "), toolbar));
    auto *radius = new QSlider(Qt::Horizontal, toolbar);
    radius->setRange(1, 200);
    radius->setValue(20);
    radius->setFixedWidth(140);
    radius->setToolTip(tr("Angular radius: 0.1° to 20°"));
    connect(radius, &QSlider::valueChanged, this, [this](int v) { view_->setBrushRadius(v / 10.0); });
    toolbar->addWidget(radius);

    statusLabel_ = new QLabel(tr("Open a 2:1 panorama to begin"), this);
    statusBar()->addPermanentWidget(statusLabel_);
    connect(view_, &SphereView::imageChanged, this, [this] {
        statusLabel_->setText(tr("360° × 180°  |  Equirectangular 2:1"));
    });
    connect(view_, &SphereView::statusMessage, this, [this](const QString &message) {
        statusBar()->showMessage(message, 6000);
    });
    connect(view_, &SphereView::modifiedChanged, this, [this](bool modified) {
        setWindowModified(modified);
        setWindowTitle(QString("PanoramaEdit — %1[*]").arg(filePath_.isEmpty() ? tr("Untitled") : QFileInfo(filePath_).fileName()));
    });
}

bool MainWindow::confirmDiscard() {
    if (!view_->isModified()) return true;
    const auto result = QMessageBox::warning(this, tr("Unsaved changes"), tr("Discard changes to the panorama?"),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    return result == QMessageBox::Discard;
}

void MainWindow::openFile() {
    if (!confirmDiscard()) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open panorama"), {}, tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
    if (path.isEmpty()) return;
    QString error;
    if (!view_->openImage(path, &error)) {
        QMessageBox::critical(this, tr("Cannot open panorama"), error);
        return;
    }
    filePath_ = path;
    setWindowTitle(QString("PanoramaEdit — %1[*]").arg(QFileInfo(filePath_).fileName()));
}

bool MainWindow::saveFile() {
    if (!view_->hasImage()) return false;
    QString proposed = filePath_.isEmpty() ? QString("panorama-edited.png") : QFileInfo(filePath_).completeBaseName() + "-edited.png";
    const QString path = QFileDialog::getSaveFileName(this, tr("Save panorama as"), proposed, tr("PNG (*.png);;JPEG (*.jpg *.jpeg)"));
    if (path.isEmpty()) return false;
    if (!view_->saveImage(path)) {
        QMessageBox::critical(this, tr("Save failed"), tr("Could not save the image."));
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
