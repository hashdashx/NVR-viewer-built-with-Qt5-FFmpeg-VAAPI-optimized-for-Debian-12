cmake_minimum_required(VERSION 3.16)
project(nvr_gui_vaapi LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt5 COMPONENTS Widgets Network REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)

add_executable(nvr_gui src/main.cpp)
target_link_libraries(nvr_gui Qt5::Widgets Qt5::Network PkgConfig::AV)
