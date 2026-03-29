cmake_minimum_required(VERSION 3.20)
project(mesh_arrangements_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# libigl — header-only, included from source via add_subdirectory.
# LIBIGL_SOURCE_DIR is set by build.py via -DLIBIGL_SOURCE_DIR=<path>.
# ---------------------------------------------------------------------------
if(NOT DEFINED LIBIGL_SOURCE_DIR OR NOT EXISTS "${LIBIGL_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "LIBIGL_SOURCE_DIR must be set to the libigl source tree.\n"
    "Run build.py instead of invoking cmake directly.")
endif()

# Enable the CGAL copyleft module (provides igl_copyleft::cgal target).
option(LIBIGL_COPYLEFT_CGAL "Use CGAL" ON)

# Prevent libigl from downloading Eigen/CGAL — vcpkg's toolchain provides them.
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)

# Find vcpkg-provided Eigen3 before adding libigl, so libigl sees it already defined.
find_package(Eigen3 CONFIG REQUIRED)

add_subdirectory("${LIBIGL_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/libigl_build" EXCLUDE_FROM_ALL)

# Dependencies — provided by vcpkg (see build.py)
find_package(CGAL CONFIG REQUIRED COMPONENTS Core)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(mesh_arrangements_runner
  src/main.cpp
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/runners/_common/cpp/include")

target_include_directories(mesh_arrangements_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
)

target_link_libraries(mesh_arrangements_runner PRIVATE
  CGAL::CGAL
  Eigen3::Eigen
  igl_copyleft::cgal
  nlohmann_json::nlohmann_json
)

message(STATUS "mesh_arrangements_runner configured — LIBIGL_SOURCE_DIR: ${LIBIGL_SOURCE_DIR}")
