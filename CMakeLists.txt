cmake_minimum_required(VERSION 3.28)
project(atem-emulator VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Multimedia MultimediaWidgets)

# The switcher itself (protocol, state, commands, macros): the emulator core,
# checked against the real ATEM Mini with atem-sweep. Also builds atem-emu,
# the same switcher without a window.
add_subdirectory(core)

set(SOURCES
    src/main.cpp
    src/Logger.h
    src/Logger.cpp
    src/AtemState.h
    src/InputSource.h
    src/InputSource.cpp
    src/Compositor.h
    src/Compositor.cpp
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
if(MSVC)
    target_compile_options(atem-emulator PRIVATE /utf-8)
endif()
target_compile_definitions(atem-emulator PRIVATE
    NOMINMAX
    ATEM_EMU_PROFILES="${CMAKE_CURRENT_SOURCE_DIR}/core/profiles")

add_dependencies(atem-emulator AtemVirtualCam)

target_link_libraries(atem-emulator PRIVATE
    atem-emu-core
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
    Qt6::MultimediaWidgets
)

# Copy Qt runtime DLLs and the switcher profiles next to the executable
add_custom_command(TARGET atem-emulator POST_BUILD
    COMMAND "${Qt6_DIR}/../../../bin/windeployqt6.exe"
            --no-translations --no-system-d3d-compiler
            "$<TARGET_FILE:atem-emulator>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/core/profiles"
            "$<TARGET_FILE_DIR:atem-emulator>/profiles"
    COMMENT "Deploying Qt runtime and switcher profiles"
)
