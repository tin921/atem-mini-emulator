cmake_minimum_required(VERSION 3.16)
project(capture-bmd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── ATEM SDK ──────────────────────────────────────────────────────────────────
# Pass on the cmake command line:
#   -DATEM_SDK_DIR="D:/cemc-sr/Blackmagic_ATEM_Switchers_SDK_10.2.1/Blackmagic ATEM Switchers SDK 10.2.1/Windows"
if(NOT DEFINED ATEM_SDK_DIR)
    message(FATAL_ERROR
        "ATEM_SDK_DIR not set. Pass it with:\n"
        "  -DATEM_SDK_DIR=\"<path to SDK Windows folder>\"")
endif()

set(ATEM_INCLUDE "${ATEM_SDK_DIR}/include")
if(NOT EXISTS "${ATEM_INCLUDE}/BMDSwitcherAPI.h")
    message(FATAL_ERROR
        "BMDSwitcherAPI.h not found at ${ATEM_INCLUDE}.\n"
        "Copy it from <SDK>/Windows/Samples/DeviceInfo/BMDSwitcherAPI.h first.")
endif()

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(capture capture-bmd.cpp)

target_include_directories(capture PRIVATE "${ATEM_INCLUDE}")

# COM support
target_link_libraries(capture PRIVATE ole32 oleaut32 comsuppw)

# Unicode
target_compile_definitions(capture PRIVATE UNICODE _UNICODE)

# Windows subsystem — console app
set_target_properties(capture PROPERTIES
    OUTPUT_NAME "capture"
    WIN32_EXECUTABLE FALSE)

# ── Install hint (optional) ───────────────────────────────────────────────────
message(STATUS "")
message(STATUS "Build output: ${CMAKE_BINARY_DIR}/Release/capture.exe")
message(STATUS "")
message(STATUS "Build commands (Developer PowerShell):")
message(STATUS "  cmake -B build -G \"Visual Studio 17 2022\" -A x64 \\")
message(STATUS "        -DATEM_SDK_DIR=\"<sdk path>\"")
message(STATUS "  cmake --build build --config Release")
message(STATUS "")
