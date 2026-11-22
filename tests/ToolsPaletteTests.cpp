#include "../src/MainWindow.h"

#include <QApplication>
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
    for (QToolButton *button : palette->findChildren<QToolButton *>()) {
        const QString name = button->text();
        foundBrush |= name == QStringLiteral("Кисть");
        foundFinger |= name == QStringLiteral("Палец");
        foundWarp |= name == QStringLiteral("Деформация");
        foundBloat |= name == QStringLiteral("Вздутие");
        foundSharpen |= name == QStringLiteral("ИИ-резкость");
        foundInpaint |= name == QStringLiteral("ИИ-дорисовка");
    }
    require(foundBrush && foundFinger && foundWarp && foundBloat,
            "Russian tool labels missing");
    require(foundSharpen && foundInpaint, "offline AI actions missing from palette");
    bool foundApply = false, foundSeamRepair = false;
    for (QPushButton *button : palette->findChildren<QPushButton *>()) {
        foundApply |= button->text() == QStringLiteral("Применить ИИ к выделению");
        foundSeamRepair |= button->text() == QStringLiteral("Сгладить шов 0°/360°");
    }
    require(foundApply && foundSeamRepair, "AI apply/seam repair controls missing");

    std::cout << "Tools palette and manual radius tests passed.\n";
    return EXIT_SUCCESS;
}
