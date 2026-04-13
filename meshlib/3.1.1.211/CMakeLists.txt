cmake_minimum_required(VERSION 3.20)
project(meshlib_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# MeshLib — built from source via add_subdirectory.
# MESHLIB_SOURCE_DIR is set by build.py via -DMESHLIB_SOURCE_DIR=<path>.
# ---------------------------------------------------------------------------
if(NOT DEFINED MESHLIB_SOURCE_DIR OR NOT EXISTS "${MESHLIB_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "MESHLIB_SOURCE_DIR must be set to the MeshLib source tree.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()

# Disable MeshLib extras we don't need.
# NOTE: these option names are best-effort guesses from the MeshLib CMakeLists.
# Adjust once we have the actual source to inspect.
set(MESHLIB_BUILD_MRVIEWER   OFF CACHE BOOL "" FORCE)
set(MESHLIB_BUILD_PYTHON     OFF CACHE BOOL "" FORCE)
set(MESHLIB_BUILD_MRCUDA     OFF CACHE BOOL "" FORCE)
set(MESHLIB_BUILD_VOXELS     OFF CACHE BOOL "" FORCE)
set(MRMESH_NO_PYTHON         ON  CACHE BOOL "" FORCE)
set(MRMESH_NO_VOXEL          ON  CACHE BOOL "" FORCE)
set(MRMESH_NO_GLTF           ON  CACHE BOOL "" FORCE)
set(MRMESH_NO_DICOM          ON  CACHE BOOL "" FORCE)
set(MRMESH_NO_JPEG           ON  CACHE BOOL "" FORCE)
set(MRMESH_NO_PNG            ON  CACHE BOOL "" FORCE)
set(MRMESH_NO_E57            ON  CACHE BOOL "" FORCE)
set(MRMESH_NO_XML            ON  CACHE BOOL "" FORCE)
set(MRMESH_NO_LAS            ON  CACHE BOOL "" FORCE)
set(BUILD_TESTING             OFF CACHE BOOL "" FORCE)

add_subdirectory("${MESHLIB_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/meshlib_build" EXCLUDE_FROM_ALL)

# Dependencies — provided by vcpkg (see build.py)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(meshlib_runner
  src/main.cpp
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/_common/cpp/include")

target_include_directories(meshlib_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
)

# NOTE: The MeshLib CMake target name is likely "MRMesh".
# Adjust if the actual target name differs once we build from source.
target_link_libraries(meshlib_runner PRIVATE
  MRMesh
  nlohmann_json::nlohmann_json
)

message(STATUS "meshlib_runner configured — MESHLIB_SOURCE_DIR: ${MESHLIB_SOURCE_DIR}")
