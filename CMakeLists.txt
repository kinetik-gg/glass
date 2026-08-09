cmake_minimum_required(VERSION 3.27)

project(kinetik-glass
    VERSION 0.8.0
    DESCRIPTION "Kinetik Glass — refractive glass surfaces for Hyprland"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB_RECURSE KINETIK_GLASS_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_library(kinetik-glass SHARED ${KINETIK_GLASS_SOURCES})

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
)

target_link_libraries(kinetik-glass PRIVATE rt PkgConfig::DEPS)
target_compile_options(kinetik-glass PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(kinetik-glass PROPERTIES PREFIX "lib")
