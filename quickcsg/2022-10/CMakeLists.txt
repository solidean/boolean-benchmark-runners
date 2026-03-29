cmake_minimum_required(VERSION 3.20)
project(quickcsg_runner LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# QuickCSG — compiled from source.
# QUICKCSG_SOURCE_DIR is set by build.py via -DQUICKCSG_SOURCE_DIR=<path>.
# ---------------------------------------------------------------------------
if(NOT DEFINED QUICKCSG_SOURCE_DIR OR NOT EXISTS "${QUICKCSG_SOURCE_DIR}")
  message(FATAL_ERROR "QUICKCSG_SOURCE_DIR must be set to the QuickCSG source tree.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()

# Glob top-level .cpp files only.
# Subdirectories (python/, experiments/, distributed/, data/) contain only
# Python/SWIG code, experiment drivers, and data — not library sources.
file(GLOB QUICKCSG_SOURCES "${QUICKCSG_SOURCE_DIR}/*.cpp")

# Exclude mesh_csg.cpp — that is the CLI entry point (defines its own main()).
list(FILTER QUICKCSG_SOURCES EXCLUDE REGEX ".*/mesh_csg\\.cpp$")

# Exclude kdtree_parallel.cpp — it is #included by kdtree.cpp, not a standalone TU.
list(FILTER QUICKCSG_SOURCES EXCLUDE REGEX ".*/kdtree_parallel\\.cpp$")

# libtess — QuickCSG's bundled polygon tessellator.
# PolygonTesselator.hpp includes <GL/glu.h>; libtess/GL/glu.h is the
# standalone drop-in that replaces the system OpenGL GLU tessellator.
# The C sources implement the GLU tessellator API without requiring OpenGL.
file(GLOB LIBTESS_SOURCES
  "${QUICKCSG_SOURCE_DIR}/libtess/*.c"
  "${QUICKCSG_SOURCE_DIR}/libtess/*.cpp"
)
# priorityq-heap.c is #included by priorityq.c — not a standalone TU.
list(FILTER LIBTESS_SOURCES EXCLUDE REGEX ".*/priorityq-heap\\.c$")
if(LIBTESS_SOURCES STREQUAL "")
  message(FATAL_ERROR "No libtess sources found under ${QUICKCSG_SOURCE_DIR}/libtess/.\n"
                      "Check that the QuickCSG clone is complete.")
endif()

# ---------------------------------------------------------------------------
# Dependencies — provided by vcpkg (see build.py and vcpkg.json)
# ---------------------------------------------------------------------------
find_package(nlohmann_json CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(quickcsg_runner
  src/main.cpp
  ${QUICKCSG_SOURCES}
  ${LIBTESS_SOURCES}
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/_common/cpp/include")

target_include_directories(quickcsg_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  $<$<BOOL:${WIN32}>:${CMAKE_CURRENT_SOURCE_DIR}/src/compat>  # MSVC stubs (unistd.h etc.)
  ${COMMON_INCLUDE_DIR}
  ${QUICKCSG_SOURCE_DIR}          # mCSG.hpp, bitvectors.hpp, kdtree.hpp, etc.
  ${QUICKCSG_SOURCE_DIR}/libtess  # provides <GL/glu.h> for PolygonTesselator
)

# QuickCSG compile-time options:
#   ENABLE_TBB — enable TBB parallel subtree traversal (requires TBB)
#   Do NOT define BVWIDTH — the default 64-bit indicator vectors support up
#   to 64 meshes, which is more than enough for binary (2-mesh) ops.
#   BVWIDTH=N would switch to wide bitvectors (N*64 bits) but is unnecessary.
target_compile_definitions(quickcsg_runner PRIVATE
  ENABLE_TBB
  $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>  # prevent <windows.h> min/max macros
)

# QuickCSG is a 2013–2015 research codebase.  It uses C++11/14 idioms that
# emit warnings under C++20.  Suppress common ones to keep the build output
# readable; our adapter code (main.cpp) still gets strict C++20.
# These flags are GCC/Clang-only; MSVC uses /W0 instead.
if(MSVC)
  set_source_files_properties(${QUICKCSG_SOURCES} ${LIBTESS_SOURCES} PROPERTIES
    COMPILE_OPTIONS "/W0"
  )
else()
  set_source_files_properties(${QUICKCSG_SOURCES} ${LIBTESS_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wno-deprecated-declarations;-Wno-unused-variable;-Wno-sign-compare"
  )
endif()

target_link_libraries(quickcsg_runner PRIVATE
  nlohmann_json::nlohmann_json
  TBB::tbb
)

message(STATUS "quickcsg_runner configured — QUICKCSG_SOURCE_DIR: ${QUICKCSG_SOURCE_DIR}")
