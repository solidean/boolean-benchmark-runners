cmake_minimum_required(VERSION 3.20)
project(meshlib_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# MeshLib — prebuilt archive extracted by build.py. The Linux and Windows
# archives have different layouts, so build.py points MESHLIB_DIR at the
# platform-appropriate prefix:
#   Linux:   download/meshlib/           (include/MeshLib/..., lib/MeshLib/libMRMesh.so*)
#   Windows: download/meshlib/install/   (include/MRMesh/..., app/<Cfg>/MRMesh.dll,
#                                         lib/<Cfg>/MRMesh.lib)
# All transitive deps (Boost, TBB, fmt, spdlog, nlohmann_json, parallel_hashmap, …)
# are bundled; no vcpkg / find_package() required.
# ---------------------------------------------------------------------------
if(WIN32)
  set(_mrmesh_header "${MESHLIB_DIR}/include/MRMesh/MRMesh.h")
else()
  set(_mrmesh_header "${MESHLIB_DIR}/include/MeshLib/MRMesh/MRMesh.h")
endif()
if(NOT DEFINED MESHLIB_DIR OR NOT EXISTS "${_mrmesh_header}")
  message(FATAL_ERROR
    "MESHLIB_DIR is not set or does not contain ${_mrmesh_header}.\n"
    "Run build.py instead of invoking cmake directly.")
endif()

add_library(MRMesh SHARED IMPORTED)

if(WIN32)
  set(_mrmesh_dll_release "${MESHLIB_DIR}/app/Release/MRMesh.dll")
  set(_mrmesh_imp_release "${MESHLIB_DIR}/lib/Release/MRMesh.lib")
  set(_mrmesh_dll_debug   "${MESHLIB_DIR}/app/Debug/MRMesh.dll")
  set(_mrmesh_imp_debug   "${MESHLIB_DIR}/lib/Debug/MRMesh.lib")
  if(NOT EXISTS "${_mrmesh_dll_release}" OR NOT EXISTS "${_mrmesh_imp_release}")
    message(FATAL_ERROR "Cannot find Release MRMesh.dll/MRMesh.lib under ${MESHLIB_DIR}/{app,lib}/Release/")
  endif()
  set_target_properties(MRMesh PROPERTIES
    IMPORTED_CONFIGURATIONS           "RELEASE"
    IMPORTED_LOCATION                 "${_mrmesh_dll_release}"
    IMPORTED_IMPLIB                   "${_mrmesh_imp_release}"
    IMPORTED_LOCATION_RELEASE         "${_mrmesh_dll_release}"
    IMPORTED_IMPLIB_RELEASE           "${_mrmesh_imp_release}"
  )
  if(EXISTS "${_mrmesh_dll_debug}" AND EXISTS "${_mrmesh_imp_debug}")
    set_property(TARGET MRMesh APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
    set_target_properties(MRMesh PROPERTIES
      IMPORTED_LOCATION_DEBUG "${_mrmesh_dll_debug}"
      IMPORTED_IMPLIB_DEBUG   "${_mrmesh_imp_debug}"
    )
  endif()
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

if(WIN32)
  target_include_directories(MRMesh INTERFACE
    "${MESHLIB_DIR}/include"
  )
else()
  target_include_directories(MRMesh INTERFACE
    "${MESHLIB_DIR}/include/MeshLib"
    "${MESHLIB_DIR}/include"
  )
endif()

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
