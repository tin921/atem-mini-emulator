cmake_minimum_required(VERSION 3.28)
project(atem-emulator VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Multimedia MultimediaWidgets)

set(SOURCES
    src/main.cpp
    src/AtemProtocol.h
    src/AtemState.h
    src/AtemState.cpp
    src/AtemServer.h
    src/AtemServer.cpp
    src/InputSource.h
    src/InputSource.cpp
    src/Compositor.h
    src/Compositor.cpp
    src/MacroEngine.h
    src/MacroEngine.cpp
    src/PreviewWidget.h
    src/PreviewWidget.cpp
    src/SourceButton.h
    src/SourceButton.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

# ── Virtual camera DLL ────────────────────────────────────────────────────────
add_library(AtemVirtualCam SHARED
    src/vcam/vcam.cpp
    src/vcam/vcam.def
)
target_include_directories(AtemVirtualCam PRIVATE src/vcam)
target_link_libraries(AtemVirtualCam PRIVATE strmiids ole32)
set_target_properties(AtemVirtualCam PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/Debug"
)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(atem-emulator WIN32 ${SOURCES})

target_include_directories(atem-emulator PRIVATE src src/vcam)

add_dependencies(atem-emulator AtemVirtualCam)

target_link_libraries(atem-emulator PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
    Qt6::MultimediaWidgets
)

# Copy Qt runtime DLLs next to the executable (Release build)
add_custom_command(TARGET atem-emulator POST_BUILD
    COMMAND "${Qt6_DIR}/../../../bin/windeployqt6.exe"
            --no-translations --no-system-d3d-compiler
            "$<TARGET_FILE:atem-emulator>"
    COMMENT "Deploying Qt runtime"
)
