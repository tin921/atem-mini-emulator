cmake_minimum_required(VERSION 3.16)
project(atem-sweep VERSION 0.1.0 LANGUAGES CXX)

# atem-sweep: exercises the Blackmagic ATEM switcher API against a real ATEM
# (or the emulator), good and bad input, records every response and the wire
# traffic, and verifies the emulator against that record.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_compile_definitions(NOMINMAX)

if(NOT ATEM_SDK_DIR)
    set(ATEM_SDK_DIR "D:/cemc-sr/Blackmagic_ATEM_Switchers_SDK_10.2.1/Blackmagic ATEM Switchers SDK 10.2.1/Windows"
        CACHE PATH "Blackmagic ATEM SDK Windows folder (contains include/BMDSwitcherAPI.h)")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_executable(atem-sweep
    src/main.cpp
    src/runner.h
    src/runner.cpp
    src/snapshot.cpp
    src/sdk.h
    src/sdk.cpp
    src/wireproxy.h
    src/wireproxy.cpp
    src/tests_common.h
    src/tests_connect.cpp
    src/tests_inputs.cpp
    src/tests_mixeffect.cpp
    src/tests_key.cpp
    src/tests_dsk_macros.cpp
    src/tests_media_device.cpp
)

target_include_directories(atem-sweep PRIVATE "${ATEM_SDK_DIR}/include" src)
target_compile_definitions(atem-sweep PRIVATE SWEEP_COVERAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/coverage")
target_link_libraries(atem-sweep PRIVATE Qt6::Core Qt6::Network ole32 oleaut32 uuid)
