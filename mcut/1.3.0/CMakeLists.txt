cmake_minimum_required(VERSION 3.20)
project(mcut_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# mcut — built from source via add_subdirectory.
# MCUT_SOURCE_DIR is set by build.py via -DMCUT_SOURCE_DIR=<path>.
# ---------------------------------------------------------------------------
if(NOT DEFINED MCUT_SOURCE_DIR OR NOT EXISTS "${MCUT_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "MCUT_SOURCE_DIR must be set to the mcut source tree.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()

# Disable mcut extras we don't need.
set(MCUT_BUILD_TUTORIALS  OFF CACHE BOOL "" FORCE)
set(MCUT_BUILD_TESTS      OFF CACHE BOOL "" FORCE)

add_subdirectory("${MCUT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/mcut_build" EXCLUDE_FROM_ALL)

# mcut/internal/frontend.h uses std::chrono without #including <chrono> (upstream bug).
# Force-include a shim header without touching mcut source.
target_include_directories(mcut PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fix")
target_compile_options(mcut PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/FImcut_extra_includes.hh>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-include mcut_extra_includes.hh>
)

# Dependencies — provided by vcpkg (see build.py)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(mcut_runner
  src/main.cpp
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/runners/_common/cpp/include")

target_include_directories(mcut_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
  ${MCUT_SOURCE_DIR}/include
)

target_link_libraries(mcut_runner PRIVATE
  mcut
  nlohmann_json::nlohmann_json
)

message(STATUS "mcut_runner configured — MCUT_SOURCE_DIR: ${MCUT_SOURCE_DIR}")
