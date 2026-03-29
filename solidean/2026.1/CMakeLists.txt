cmake_minimum_required(VERSION 3.20)
project(solidean_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies — provided by vcpkg (see build.py)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Solidean SDK — SOLIDEAN_DIR must point at the extracted SDK root
# (set by build.py via -DSOLIDEAN_DIR=<path>)
# ---------------------------------------------------------------------------
if(NOT DEFINED SOLIDEAN_DIR OR NOT EXISTS "${SOLIDEAN_DIR}/lang/cpp17")
    message(FATAL_ERROR
        "SOLIDEAN_DIR is not set or does not contain lang/cpp17.\n"
        "Pass -DSOLIDEAN_DIR=<path/to/solidean/sdk> to cmake.\n"
        "Run build.py to download and set this up automatically.")
endif()

add_subdirectory(${SOLIDEAN_DIR}/lang/cpp17 solidean_cpp17)

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(solidean_runner
    src/main.cpp
)

if(NOT DEFINED PROJECT_ROOT)
    message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/_common/cpp/include")

target_include_directories(solidean_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${COMMON_INCLUDE_DIR}
)

target_link_libraries(solidean_runner PRIVATE
    Solidean::Cpp17
    nlohmann_json::nlohmann_json
)

if(UNIX)
  set_target_properties(solidean_runner PROPERTIES
    BUILD_RPATH  "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()

message(STATUS "solidean_runner configured — SOLIDEAN_DIR: ${SOLIDEAN_DIR}")
