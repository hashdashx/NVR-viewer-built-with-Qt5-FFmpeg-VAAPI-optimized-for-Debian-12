cmake_minimum_required(VERSION 3.10)
project(nvr_gui_vaapi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

# ----------------------------
# Qt5
# ----------------------------
find_package(Qt5 REQUIRED COMPONENTS Core Widgets Network)

# ----------------------------
# FFmpeg / VAAPI
# ----------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libavutil
  libswscale
)

# ----------------------------
# Include & Link
# ----------------------------
include_directories(
  ${Qt5Core_INCLUDE_DIRS}
  ${Qt5Widgets_INCLUDE_DIRS}
  ${Qt5Network_INCLUDE_DIRS}
  ${FFMPEG_INCLUDE_DIRS}
)

add_executable(nvr_gui
  src/main.cpp
)

target_link_libraries(nvr_gui
  Qt5::Core
  Qt5::Widgets
  Qt5::Network
  PkgConfig::FFMPEG
)

# ----------------------------
# Install hints for Samsung SBB A4 (optional)
# ----------------------------
message(STATUS "âœ… Build config ready for VAAPI Qt5 (AMD / Samsung SBB A4)")

# typical ffmpeg dev packages:
#   sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libavfilter-dev
# typical qt dev packages:
#   sudo apt install qtbase5-dev qt5-qmake qtbase5-dev-tools libqt5network5
