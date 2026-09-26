cmake_minimum_required(VERSION 3.16)
project(atem-emu VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

# The emulator core: no UI, so the headless tool and a GUI can share it.
add_library(atem-emu-core STATIC
    src/fields.h src/fields.cpp
    src/device.h src/device.cpp
    src/server.h src/server.cpp
)
target_include_directories(atem-emu-core PUBLIC src)
target_link_libraries(atem-emu-core PUBLIC Qt6::Core Qt6::Network)

add_executable(atem-emu src/main.cpp)
target_link_libraries(atem-emu PRIVATE atem-emu-core)
target_compile_definitions(atem-emu PRIVATE ATEM_EMU_PROFILES="${CMAKE_CURRENT_SOURCE_DIR}/profiles")

install(TARGETS atem-emu RUNTIME DESTINATION .)
install(DIRECTORY profiles DESTINATION .)
