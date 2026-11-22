#pragma once
#include <QMainWindow>
#include <QString>
#include "AiEngine.h"

template <typename T> class QFutureWatcher;

class SphereView;
class QAction;
class QLabel;
class QPushButton;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    bool confirmDiscard();
    void openFile();
    bool saveFile();
    void startAiRestoration();
    SphereView *view_ = nullptr;
    QString filePath_;
    QAction *navigateAction_ = nullptr;
    QAction *brushAction_ = nullptr;
    QAction *eraserAction_ = nullptr;
    QAction *stampAction_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QPushButton *applyAiButton_ = nullptr;
    QFutureWatcher<AiEngine::Result> *aiWatcher_ = nullptr;
};
