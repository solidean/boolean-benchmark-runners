cmake_minimum_required(VERSION 3.20)
project(cork_runner LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Cork — compiled from source.
# Cork uses SCons, not CMake, so we glob its source files directly.
# CORK_SOURCE_DIR is set by build.py via -DCORK_SOURCE_DIR=<path>.
# ---------------------------------------------------------------------------
if(NOT DEFINED CORK_SOURCE_DIR OR NOT EXISTS "${CORK_SOURCE_DIR}")
  message(FATAL_ERROR "CORK_SOURCE_DIR must be set to the cork source tree.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()
file(TO_CMAKE_PATH "${CORK_SOURCE_DIR}" CORK_SOURCE_DIR)

# Glob all .cpp files under cork/src/ (excluding main entry points if any).
# Cork's library sources live in src/ alongside the public header cork.h.
file(GLOB_RECURSE CORK_SOURCES
  "${CORK_SOURCE_DIR}/src/*.cpp"
)

# Exclude top-level drivers (cork's own main.cpp and off2obj.cpp have their own main()).
list(FILTER CORK_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
list(FILTER CORK_SOURCES EXCLUDE REGEX ".*/off2obj\\.cpp$")

# triangle.c is Shewchuk's triangulator.  Define TRILIBRARY so it compiles as a
# library (no standalone main); without it, triangle.c defines its own main().
set(CORK_C_SOURCES "${CORK_SOURCE_DIR}/src/isct/triangle.c")
set_source_files_properties("${CORK_SOURCE_DIR}/src/isct/triangle.c" PROPERTIES
  # TRILIBRARY: compile as a library (no standalone main).
  # NO_TIMER: skip the <sys/time.h> timing code — that header is POSIX-only.
  # NOMINMAX / _USE_MATH_DEFINES: Windows MSVC compatibility.
  COMPILE_DEFINITIONS "TRILIBRARY;NO_TIMER;NOMINMAX;_USE_MATH_DEFINES;ANSI_DECLARATORS"
  # -std=gnu89: K&R-style C; modern standards reject it.  MSVC doesn't support
  # this flag but handles K&R C natively, so skip it on MSVC.
  COMPILE_OPTIONS "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-std=gnu89>"
)

# ---------------------------------------------------------------------------
# Dependencies — provided by vcpkg (see build.py and vcpkg.json)
# ---------------------------------------------------------------------------
find_package(nlohmann_json CONFIG REQUIRED)

# GMP / MPIR — cork requires multi-precision arithmetic.
# Cork's source already handles the platform split:
#   Windows → #include <mpir.h>   (vcpkg mpir)
#   Other   → #include <gmp.h>    (vcpkg gmp)
# Neither package ships a CMake config, so use find_library/find_path directly.
# The vcpkg toolchain adds the installed prefix to CMAKE_PREFIX_PATH so both
# headers and libs are found without pkg-config.
if(WIN32)
  # vcpkg mpir bundles both C and C++ APIs into a single mpir.lib.
  find_path(GMP_INCLUDE_DIR NAMES mpir.h REQUIRED)
  find_library(GMP_LIBRARY  NAMES mpir   REQUIRED)

  add_library(gmp_imported UNKNOWN IMPORTED)
  set_target_properties(gmp_imported PROPERTIES
    IMPORTED_LOCATION             "${GMP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${GMP_INCLUDE_DIR}"
  )
  # Alias so the link line below is platform-independent.
  add_library(gmpxx_imported ALIAS gmp_imported)
else()
  find_path(GMP_INCLUDE_DIR   NAMES gmp.h   REQUIRED)
  find_path(GMPXX_INCLUDE_DIR NAMES gmpxx.h REQUIRED)
  find_library(GMP_LIBRARY   NAMES gmp   REQUIRED)
  find_library(GMPXX_LIBRARY NAMES gmpxx REQUIRED)

  add_library(gmp_imported UNKNOWN IMPORTED)
  set_target_properties(gmp_imported PROPERTIES
    IMPORTED_LOCATION             "${GMP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${GMP_INCLUDE_DIR}"
  )

  add_library(gmpxx_imported UNKNOWN IMPORTED)
  set_target_properties(gmpxx_imported PROPERTIES
    IMPORTED_LOCATION             "${GMPXX_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${GMPXX_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES      gmp_imported
  )
endif()

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(cork_runner
  src/main.cpp
  ${CORK_SOURCES}
  ${CORK_C_SOURCES}
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
file(TO_CMAKE_PATH "${PROJECT_ROOT}" PROJECT_ROOT)
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/_common/cpp/include")

# Cork's headers live in per-feature subdirectories (mesh/, math/, rawmesh/, …).
# Collect them so relative includes like #include "mesh.h" resolve correctly.
file(GLOB _cork_subdirs LIST_DIRECTORIES true "${CORK_SOURCE_DIR}/src/*")
set(_cork_include_dirs "${CORK_SOURCE_DIR}/src")
foreach(_d IN LISTS _cork_subdirs)
  if(IS_DIRECTORY "${_d}")
    list(APPEND _cork_include_dirs "${_d}")
  endif()
endforeach()

target_include_directories(cork_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
  ${_cork_include_dirs}
)

# Cork (2013) uses template-constructor syntax (Ray3<N>()) that was
# deprecated/broken in C++20.  Compile those TUs with C++17; our
# adapter (main.cpp) still gets C++20 from CMAKE_CXX_STANDARD.
# NOMINMAX prevents Windows.h (included via winsock.h in prelude.h) from
# defining min/max macros that would clobber cork's free-function templates.
# _USE_MATH_DEFINES exposes M_PI on MSVC (it's in POSIX but not ISO C++).
# The -std=c++17 flag is GCC/Clang syntax; on MSVC the standard is already
# controlled by CMAKE_CXX_STANDARD so skip the flag there.
set_source_files_properties(${CORK_SOURCES} PROPERTIES
  COMPILE_DEFINITIONS "NOMINMAX;_USE_MATH_DEFINES"
  COMPILE_OPTIONS     "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-std=c++17>"
)

target_link_libraries(cork_runner PRIVATE
  nlohmann_json::nlohmann_json
  gmp_imported
  gmpxx_imported
)

message(STATUS "cork_runner configured — CORK_SOURCE_DIR: ${CORK_SOURCE_DIR}")
