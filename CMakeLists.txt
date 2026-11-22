cmake_minimum_required(VERSION 3.24)
project(PanoramaEdit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
qt_standard_project_setup()

qt_add_executable(PanoramaEdit WIN32
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/SphereView.h
    src/SphereView.cpp
)
target_link_libraries(PanoramaEdit PRIVATE Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
set_target_properties(PanoramaEdit PROPERTIES WIN32_EXECUTABLE TRUE)
install(TARGETS PanoramaEdit RUNTIME DESTINATION .)
