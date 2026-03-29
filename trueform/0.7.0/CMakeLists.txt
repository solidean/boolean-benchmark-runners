cmake_minimum_required(VERSION 3.20)
project(trueform_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Trueform — header-only; TRUEFORM_SOURCE_DIR is set by build.py.
# ---------------------------------------------------------------------------
if(NOT DEFINED TRUEFORM_SOURCE_DIR OR NOT EXISTS "${TRUEFORM_SOURCE_DIR}")
  message(FATAL_ERROR "TRUEFORM_SOURCE_DIR must be set to the trueform source tree.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()

# Dependencies — provided by vcpkg (see build.py)
find_package(TBB CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(trueform_runner
  src/main.cpp
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/_common/cpp/include")

target_include_directories(trueform_runner PRIVATE
  "${TRUEFORM_SOURCE_DIR}/include"
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
)

target_link_libraries(trueform_runner PRIVATE
  TBB::tbb
  nlohmann_json::nlohmann_json
)

message(STATUS "trueform_runner configured — TRUEFORM_SOURCE_DIR: ${TRUEFORM_SOURCE_DIR}")
