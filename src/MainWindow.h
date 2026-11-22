#pragma once
#include <QMainWindow>
#include <QString>

class SphereView;
class QAction;
class QLabel;

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
    SphereView *view_ = nullptr;
    QString filePath_;
    QAction *navigateAction_ = nullptr;
    QAction *brushAction_ = nullptr;
    QAction *eraserAction_ = nullptr;
    QAction *stampAction_ = nullptr;
    QLabel *statusLabel_ = nullptr;
};
