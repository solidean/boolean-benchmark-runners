cmake_minimum_required(VERSION 3.20)
project(meshlib_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# MeshLib — pre-built in step 1 (see build.py).
# MESHLIB_SOURCE_DIR  = MeshLib source tree (for headers)
# MESHLIB_BUILD_DIR   = MeshLib cmake build directory (for libraries)
# ---------------------------------------------------------------------------
if(NOT DEFINED MESHLIB_SOURCE_DIR OR NOT EXISTS "${MESHLIB_SOURCE_DIR}/source/MRMesh")
  message(FATAL_ERROR "MESHLIB_SOURCE_DIR must point to the MeshLib source tree.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()
if(NOT DEFINED MESHLIB_BUILD_DIR)
  message(FATAL_ERROR "MESHLIB_BUILD_DIR must point to the MeshLib build directory.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()

# Create an imported target for the pre-built MRMesh shared library.
add_library(MRMesh SHARED IMPORTED)

# Find the .so / .dll
file(GLOB _mrmesh_lib "${MESHLIB_BUILD_DIR}/bin/libMRMesh.so*")
if(NOT _mrmesh_lib)
  file(GLOB _mrmesh_lib "${MESHLIB_BUILD_DIR}/bin/MRMesh.dll")
endif()
if(NOT _mrmesh_lib)
  message(FATAL_ERROR "Cannot find libMRMesh in ${MESHLIB_BUILD_DIR}/bin/")
endif()
list(GET _mrmesh_lib 0 _mrmesh_lib_path)
set_target_properties(MRMesh PROPERTIES
  IMPORTED_LOCATION "${_mrmesh_lib_path}"
  IMPORTED_NO_SONAME TRUE
)

# MeshLib headers: source/MRMesh, source/MRPch, and source/ (for MRMesh/MRMeshFwd.h style includes)
target_include_directories(MRMesh INTERFACE
  "${MESHLIB_SOURCE_DIR}/source"
  "${MESHLIB_SOURCE_DIR}/source/MRMesh"
  "${MESHLIB_SOURCE_DIR}/source/MRPch"
)

# Dependencies — provided by vcpkg
find_package(nlohmann_json CONFIG REQUIRED)
find_package(parallel-hashmap CONFIG REQUIRED)

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

target_link_libraries(meshlib_runner PRIVATE
  MRMesh
  nlohmann_json::nlohmann_json
)

# Ensure the runner can find libMRMesh.so at runtime
set_target_properties(meshlib_runner PROPERTIES
  BUILD_RPATH "${MESHLIB_BUILD_DIR}/bin"
  INSTALL_RPATH "$ORIGIN"
)

message(STATUS "meshlib_runner configured — MESHLIB_SOURCE_DIR: ${MESHLIB_SOURCE_DIR}")
message(STATUS "  MRMesh library: ${_mrmesh_lib_path}")
