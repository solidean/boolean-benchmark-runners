cmake_minimum_required(VERSION 3.20)
project(geogram_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Geogram — built from source via add_subdirectory.
# GEOGRAM_SOURCE_DIR is set by build.py via -DGEOGRAM_SOURCE_DIR=<path>.
# ---------------------------------------------------------------------------
if(NOT DEFINED GEOGRAM_SOURCE_DIR OR NOT EXISTS "${GEOGRAM_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "GEOGRAM_SOURCE_DIR must be set to the geogram source tree.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()

# Disable geogram components we don't need.
set(GEOGRAM_WITH_GRAPHICS OFF CACHE BOOL "" FORCE)
set(GEOGRAM_WITH_LUA      OFF CACHE BOOL "" FORCE)
set(GEOGRAM_WITH_HLBFGS   OFF CACHE BOOL "" FORCE)
set(GEOGRAM_WITH_TETGEN   OFF CACHE BOOL "" FORCE)
set(GEOGRAM_WITH_TRIANGLE OFF CACHE BOOL "" FORCE)

add_subdirectory("${GEOGRAM_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/geogram_build" EXCLUDE_FROM_ALL)

# Dependencies — provided by vcpkg (see build.py)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(geogram_runner
  src/main.cpp
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/runners/_common/cpp/include")

target_include_directories(geogram_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
)

target_link_libraries(geogram_runner PRIVATE
  geogram
  nlohmann_json::nlohmann_json
)

message(STATUS "geogram_runner configured — GEOGRAM_SOURCE_DIR: ${GEOGRAM_SOURCE_DIR}")
