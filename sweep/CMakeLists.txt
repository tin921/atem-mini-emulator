cmake_minimum_required(VERSION 3.16)
project(atem-sweep VERSION 0.1.0 LANGUAGES CXX)

# atem-sweep: exercises the Blackmagic ATEM switcher API against a real ATEM
# (or the emulator), good and bad input, records every response and the wire
# traffic, and verifies the emulator against that record.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8> $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
add_compile_definitions(NOMINMAX)

if(NOT ATEM_SDK_DIR)
    set(ATEM_SDK_DIR "D:/cemc-sr/Blackmagic_ATEM_Switchers_SDK_10.2.1/Blackmagic ATEM Switchers SDK 10.2.1/Windows"
        CACHE PATH "Blackmagic ATEM SDK Windows folder (contains include/BMDSwitcherAPI.h)")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets)
set(CMAKE_AUTOMOC ON)

add_executable(atem-sweep
    src/groups.h
    src/main.cpp
    src/backup.h
    src/backup.cpp
    src/runner.h
    src/runner.cpp
    src/snapshot.cpp
    src/sdk.h
    src/sdk.cpp
    src/wireproxy.h
    src/wireproxy.cpp
    src/tests_common.h
    src/tests_connect.cpp
    src/tests_probe.cpp
    src/tests_access.h
    src/tests_access.cpp
    src/tests_generated.cpp
    src/tests_manual.cpp
    src/tests_storage.cpp
    src/transfer.h
    src/probe_ifaces.inc
    src/tests_inputs.cpp
    src/tests_mixeffect.cpp
    src/tests_key.cpp
    src/tests_dsk_macros.cpp
    src/tests_media_device.cpp
)

target_include_directories(atem-sweep PRIVATE "${ATEM_SDK_DIR}/include" src)
target_compile_definitions(atem-sweep PRIVATE SWEEP_COVERAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/coverage"
                                              SWEEP_BACKUP_DIR="${CMAKE_CURRENT_SOURCE_DIR}/backups")
target_link_libraries(atem-sweep PRIVATE Qt6::Core Qt6::Gui Qt6::Network ole32 oleaut32 uuid)

# atem-sweep-gui: the sweep as a coverage map. Runs atem-sweep (next to it)
# and, for checks against the emulator, atem-emu from the emulator's build.
add_executable(atem-sweep-gui WIN32
    gui/main.cpp
    gui/model.h
    gui/model.cpp
    gui/widgets.h
    gui/widgets.cpp
    gui/window.h
    gui/window.cpp
)
target_include_directories(atem-sweep-gui PRIVATE src gui)
target_compile_definitions(atem-sweep-gui PRIVATE
    SWEEP_COVERAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/coverage"
    SWEEP_BACKUP_DIR="${CMAKE_CURRENT_SOURCE_DIR}/backups"
    SWEEP_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
    EMU_EXE="${CMAKE_CURRENT_SOURCE_DIR}/../build-gui/core/Release/atem-emu.exe"
    EMU_PROFILES="${CMAKE_CURRENT_SOURCE_DIR}/../core/profiles")
target_link_libraries(atem-sweep-gui PRIVATE Qt6::Widgets iphlpapi ws2_32)
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qtbin "${_qmake}" DIRECTORY)
target_compile_definitions(atem-sweep-gui PRIVATE QT_BIN_DIR="${_qtbin}")
add_dependencies(atem-sweep-gui atem-sweep)

# The Qt runtime next to atem-sweep-gui, so it starts without Qt on PATH.
find_program(WINDEPLOYQT windeployqt6 windeployqt HINTS "${_qtbin}")
if(WINDEPLOYQT)
    add_custom_command(TARGET atem-sweep-gui POST_BUILD
        COMMAND "${WINDEPLOYQT}" --no-translations --no-compiler-runtime "$<TARGET_FILE:atem-sweep-gui>"
        COMMENT "windeployqt atem-sweep-gui")
endif()
