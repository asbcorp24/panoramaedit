#include "../src/MainWindow.h"

#include <QApplication>
#include <QAction>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSlider>
#include <QToolButton>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    MainWindow window;

    auto *palette = window.findChild<QDockWidget *>("ToolsPalette");
    require(palette != nullptr, "tools palette not found");
    require(palette->isFloating(), "tools palette must start floating");

    auto *manualRadius = palette->findChild<QDoubleSpinBox *>();
    require(manualRadius != nullptr, "manual radius entry missing");
    require(std::abs(manualRadius->value() - 2.0) < 1e-6,
            "manual radius default differs from brush default");

    QSlider *radiusSlider = nullptr;
    for (QSlider *slider : palette->findChildren<QSlider *>()) {
        if (slider->maximum() == 300) radiusSlider = slider;
    }
    require(radiusSlider != nullptr, "radius slider missing");

    manualRadius->setValue(12.7);
    require(radiusSlider->value() == 127, "manual radius did not update slider");
    radiusSlider->setValue(95);
    require(std::abs(manualRadius->value() - 9.5) < 1e-6,
            "slider did not update manual radius");
    manualRadius->setValue(30.0);
    require(radiusSlider->value() == 300, "30 degree radius is unavailable");

    bool foundBrush = false, foundFinger = false, foundWarp = false, foundBloat = false;
    bool foundSharpen = false, foundInpaint = false;
    QToolButton *brushButton = nullptr;
    QToolButton *navigateButton = nullptr;
    int iconTools = 0;
    for (QToolButton *button : palette->findChildren<QToolButton *>()) {
        const QString name = button->accessibleName();
        require(button->toolButtonStyle() == Qt::ToolButtonIconOnly,
                "tool shows text rather than icon");
        require(!button->icon().isNull(), "tool has no icon");
        require(button->defaultAction() != nullptr, "tool has no action");
        require(!button->defaultAction()->shortcut().isEmpty(), "tool shortcut lost");
        require(button->toolTip().contains(name), "Russian tool tooltip missing");
        ++iconTools;
        if (name == QStringLiteral("Кисть")) brushButton = button;
        if (name == QStringLiteral("Навигация")) navigateButton = button;
        foundBrush |= name == QStringLiteral("Кисть");
        foundFinger |= name == QStringLiteral("Палец");
        foundWarp |= name == QStringLiteral("Деформация");
        foundBloat |= name == QStringLiteral("Вздутие");
        foundSharpen |= name == QStringLiteral("ИИ-резкость");
        foundInpaint |= name == QStringLiteral("ИИ-дорисовка");
    }
    require(foundBrush && foundFinger && foundWarp && foundBloat,
            "Russian tool accessible names missing");
    require(foundSharpen && foundInpaint, "offline AI actions missing from palette");
    require(iconTools == 10, "unexpected icon tool count");
    require(navigateButton && navigateButton->isChecked(),
            "navigation icon should be selected by default");
    require(brushButton && brushButton->toolTip().contains("(B)"),
            "brush shortcut missing from tooltip");
    brushButton->click();
    require(brushButton->isChecked() && !navigateButton->isChecked(),
            "icon selection did not switch to brush");
    bool foundApply = false, foundSeamRepair = false, foundColor = false, foundClear = false;
    int iconActions = 0;
    for (QPushButton *button : palette->findChildren<QPushButton *>()) {
        require(button->text().isEmpty(), "action button displays text");
        require(!button->icon().isNull(), "action button has no icon");
        require(button->toolTip().contains(button->accessibleName()),
                "Russian action tooltip missing");
        const QString name = button->accessibleName();
        foundApply |= name == QStringLiteral("Применить ИИ к выделению");
        foundSeamRepair |= name == QStringLiteral("Сгладить шов 0°/360°");
        foundColor |= name == QStringLiteral("Цвет кисти");
        foundClear |= name == QStringLiteral("Очистить ИИ-выделение");
        ++iconActions;
    }
    require(iconActions == 4 && foundApply && foundSeamRepair && foundColor && foundClear,
            "icon-only AI/color/seam controls missing");

    std::cout << "Tools palette and manual radius tests passed.\n";
    return EXIT_SUCCESS;
}
