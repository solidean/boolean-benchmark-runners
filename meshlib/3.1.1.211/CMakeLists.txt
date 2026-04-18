cmake_minimum_required(VERSION 3.20)
project(meshlib_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# MeshLib — prebuilt archive extracted by build.py into download/meshlib/.
# MESHLIB_DIR points at that directory (vcpkg-style layout: include/, lib/, bin/).
# All transitive deps (Boost, TBB, fmt, spdlog, nlohmann_json, parallel_hashmap, …)
# are bundled under include/ and lib/; no vcpkg / find_package() required.
# ---------------------------------------------------------------------------
if(NOT DEFINED MESHLIB_DIR OR NOT EXISTS "${MESHLIB_DIR}/include/MeshLib/MRMesh/MRMesh.h")
  message(FATAL_ERROR
    "MESHLIB_DIR is not set or does not contain include/MeshLib/MRMesh/MRMesh.h.\n"
    "Run build.py instead of invoking cmake directly.")
endif()

add_library(MRMesh SHARED IMPORTED)

if(WIN32)
  # TODO: verify the Windows layout against MeshLibDist_v<ver>.zip on a Windows machine.
  set(_mrmesh_dll "${MESHLIB_DIR}/bin/MRMesh.dll")
  set(_mrmesh_imp "${MESHLIB_DIR}/lib/MRMesh.lib")
  if(NOT EXISTS "${_mrmesh_dll}" OR NOT EXISTS "${_mrmesh_imp}")
    message(FATAL_ERROR "Cannot find MRMesh.dll/MRMesh.lib under ${MESHLIB_DIR}")
  endif()
  set_target_properties(MRMesh PROPERTIES
    IMPORTED_LOCATION "${_mrmesh_dll}"
    IMPORTED_IMPLIB   "${_mrmesh_imp}"
  )
else()
  file(GLOB _mrmesh_lib "${MESHLIB_DIR}/lib/MeshLib/libMRMesh.so*")
  if(NOT _mrmesh_lib)
    message(FATAL_ERROR "Cannot find libMRMesh.so* under ${MESHLIB_DIR}/lib/MeshLib/")
  endif()
  list(GET _mrmesh_lib 0 _mrmesh_lib_path)
  set_target_properties(MRMesh PROPERTIES
    IMPORTED_LOCATION  "${_mrmesh_lib_path}"
    IMPORTED_NO_SONAME TRUE
  )
endif()

target_include_directories(MRMesh INTERFACE
  "${MESHLIB_DIR}/include/MeshLib"
  "${MESHLIB_DIR}/include"
)

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
)

if(UNIX)
  # build.py copies libMRMesh.so + transitive .so's next to the executable,
  # so $ORIGIN makes the built binary self-contained. BUILD_WITH_INSTALL_RPATH
  # bakes INSTALL_RPATH into the build output (no post-install rewrite needed).
  set_target_properties(meshlib_runner PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH            "$ORIGIN"
  )
endif()

message(STATUS "meshlib_runner configured — MESHLIB_DIR: ${MESHLIB_DIR}")
if(NOT WIN32)
  message(STATUS "  MRMesh library: ${_mrmesh_lib_path}")
endif()
