cmake_minimum_required(VERSION 3.20)
project(manifold_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Manifold — built from source via add_subdirectory.
# MANIFOLD_SOURCE_DIR is set by build.py via -DMANIFOLD_SOURCE_DIR=<path>.
# ---------------------------------------------------------------------------
if(NOT DEFINED MANIFOLD_SOURCE_DIR OR NOT EXISTS "${MANIFOLD_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "MANIFOLD_SOURCE_DIR must be set to the manifold source tree.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()

# Disable manifold extras we don't need.
set(MANIFOLD_TEST    OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CBIND   OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PYBIND  OFF CACHE BOOL "" FORCE)
set(MANIFOLD_EXPORT  OFF CACHE BOOL "" FORCE)
set(MANIFOLD_FUZZ    OFF CACHE BOOL "" FORCE)
set(MANIFOLD_DOWNLOADS OFF CACHE BOOL "" FORCE)

add_subdirectory("${MANIFOLD_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/manifold_build" EXCLUDE_FROM_ALL)

# Dependencies — provided by vcpkg (see build.py)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(manifold_runner
  src/main.cpp
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/runners/_common/cpp/include")

target_include_directories(manifold_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
)

target_link_libraries(manifold_runner PRIVATE
  manifold
  nlohmann_json::nlohmann_json
)

message(STATUS "manifold_runner configured — MANIFOLD_SOURCE_DIR: ${MANIFOLD_SOURCE_DIR}")
