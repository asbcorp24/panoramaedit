cmake_minimum_required(VERSION 3.24)
project(PanoramaEdit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent)
qt_standard_project_setup()

# The Windows package includes native ONNX Runtime + DirectML. Models are
# downloaded separately once, and inference never needs Python.
add_library(PanoramaEditOrt INTERFACE)
if(WIN32)
    set(PANORAMAEDIT_ONNX_DIR "" CACHE PATH "Extracted Microsoft.ML.OnnxRuntime.DirectML NuGet package")
    if(NOT PANORAMAEDIT_ONNX_DIR)
        include(FetchContent)
        FetchContent_Declare(onnxruntime_directml
            URL "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.4"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(onnxruntime_directml)
        set(PANORAMAEDIT_ONNX_DIR "${onnxruntime_directml_SOURCE_DIR}")
    endif()
    set(ORT_NATIVE_DIR "${PANORAMAEDIT_ONNX_DIR}/runtimes/win-x64/native")
    if(NOT EXISTS "${ORT_NATIVE_DIR}/onnxruntime.lib")
        message(FATAL_ERROR "ONNX Runtime import library missing: ${ORT_NATIVE_DIR}/onnxruntime.lib")
    endif()
    target_include_directories(PanoramaEditOrt INTERFACE "${PANORAMAEDIT_ONNX_DIR}/build/native/include")
    target_link_libraries(PanoramaEditOrt INTERFACE "${ORT_NATIVE_DIR}/onnxruntime.lib")
    file(GLOB ORT_RUNTIME_DLLS "${ORT_NATIVE_DIR}/*.dll")
    if(NOT ORT_RUNTIME_DLLS)
        message(FATAL_ERROR "ONNX Runtime DLLs missing in ${ORT_NATIVE_DIR}")
    endif()

    # ONNX Runtime's DirectML NuGet package does not bundle DirectML.dll.
    # Ship Microsoft's redistributable beside the EXE so GPU support does not
    # depend on whichever (possibly older) copy Windows happens to provide.
    set(PANORAMAEDIT_DML_DIR "" CACHE PATH "Extracted Microsoft.AI.DirectML NuGet package")
    if(NOT PANORAMAEDIT_DML_DIR)
        include(FetchContent)
        FetchContent_Declare(directml_redist
            URL "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(directml_redist)
        set(PANORAMAEDIT_DML_DIR "${directml_redist_SOURCE_DIR}")
    endif()
    set(DIRECTML_DLL "${PANORAMAEDIT_DML_DIR}/bin/x64-win/DirectML.dll")
    if(NOT EXISTS "${DIRECTML_DLL}")
        message(FATAL_ERROR "DirectML redistributable missing: ${DIRECTML_DLL}")
    endif()
    list(APPEND ORT_RUNTIME_DLLS "${DIRECTML_DLL}")
    install(FILES ${ORT_RUNTIME_DLLS} DESTINATION .)
endif()

function(configure_native_ai target)
    target_link_libraries(${target} PRIVATE PanoramaEditOrt)
    if(WIN32)
        foreach(runtime_dll IN LISTS ORT_RUNTIME_DLLS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime_dll}" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endforeach()
    endif()
endfunction()

qt_add_executable(PanoramaEdit WIN32
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/ToolIcons.h
    src/ToolIcons.cpp
    src/AiEngine.h
    src/AiEngine.cpp
    src/SphereView.h
    src/SphereView.cpp
    src/SphericalLiquify.h
    src/SphericalLiquify.cpp
    src/SphericalAI.h
    src/SphericalAI.cpp
    src/SphericalRaster.h
    src/SphericalRaster.cpp
)
target_link_libraries(PanoramaEdit PRIVATE Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Concurrent)
configure_native_ai(PanoramaEdit)
set_target_properties(PanoramaEdit PROPERTIES WIN32_EXECUTABLE TRUE)
install(TARGETS PanoramaEdit RUNTIME DESTINATION .)

include(CTest)
if(BUILD_TESTING)
    add_executable(PanoramaEditRasterTests
        tests/SphericalRasterTests.cpp
        src/SphericalRaster.cpp
        src/SphericalRaster.h
        src/SphericalLiquify.cpp
        src/SphericalLiquify.h
        src/SphericalAI.h
        src/SphericalAI.cpp
    )
    target_link_libraries(PanoramaEditRasterTests PRIVATE Qt6::Gui)
    add_test(NAME SphereRaster COMMAND PanoramaEditRasterTests)

    add_executable(PanoramaEditAITests
        tests/SphericalAITests.cpp
        src/SphericalAI.h
        src/SphericalAI.cpp
        src/SphericalRaster.h
        src/SphericalRaster.cpp
    )
    target_link_libraries(PanoramaEditAITests PRIVATE Qt6::Gui)
    add_test(NAME SphericalAI COMMAND PanoramaEditAITests)

    add_executable(PanoramaEditNativeAiTests
        tests/NativeAiTests.cpp
        src/AiEngine.h
        src/AiEngine.cpp
    )
    target_link_libraries(PanoramaEditNativeAiTests PRIVATE Qt6::Gui)
    configure_native_ai(PanoramaEditNativeAiTests)
    add_test(NAME NativeAI COMMAND PanoramaEditNativeAiTests)

    add_executable(PanoramaEditPaletteTests
        tests/ToolsPaletteTests.cpp
        src/MainWindow.h
        src/MainWindow.cpp
        src/ToolIcons.h
        src/ToolIcons.cpp
        src/AiEngine.h
        src/AiEngine.cpp
        src/SphereView.h
        src/SphereView.cpp
        src/SphericalLiquify.h
        src/SphericalLiquify.cpp
        src/SphericalAI.h
        src/SphericalAI.cpp
        src/SphericalRaster.h
        src/SphericalRaster.cpp
    )
    target_link_libraries(PanoramaEditPaletteTests PRIVATE
        Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Concurrent)
    configure_native_ai(PanoramaEditPaletteTests)
    add_test(NAME ToolsPalette COMMAND PanoramaEditPaletteTests)
    set_tests_properties(ToolsPalette PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
