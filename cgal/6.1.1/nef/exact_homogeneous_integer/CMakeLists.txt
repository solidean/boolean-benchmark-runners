cmake_minimum_required(VERSION 3.20)
project(nef_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Dependencies — provided by vcpkg (toolchain passed via -DCMAKE_TOOLCHAIN_FILE)
# vcpkg.json declares: boost, gmp, mpfr, nlohmann-json
# ---------------------------------------------------------------------------
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# CGAL — header-only in 6.x; CGAL_DIR must point at the CGAL source tree
# (set by build.py via -DCGAL_DIR=<path>)
# GMP, MPFR, and Boost are provided by vcpkg.
# ---------------------------------------------------------------------------
find_package(CGAL REQUIRED)

# Exact arithmetic support (CGAL_Core / GMP integers)
# CGAL::CGAL brings in GMP/MPFR transitively when they are found.
# If the build host only has CGAL without CGAL_Core compiled, the Exact_integer
# typedef will fall back to CGAL::MP_Float which is slower but still exact.

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(nef_runner
  src/main.cpp
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/runners/_common/cpp/include")

target_include_directories(nef_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
)

target_link_libraries(nef_runner PRIVATE
  CGAL::CGAL
  nlohmann_json::nlohmann_json
)

# CGAL demands NDEBUG in release builds to avoid expensive assertion checks.
target_compile_definitions(nef_runner PRIVATE
  $<$<CONFIG:Release>:NDEBUG>
  $<$<CONFIG:RelWithDebInfo>:NDEBUG>
)

# Warn loudly in debug builds to catch logic errors during development.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nef_runner PRIVATE
    $<$<CONFIG:Debug>:-Wall -Wextra -Wpedantic>
    $<$<CONFIG:Release>:-O3>
  )
endif()

message(STATUS "nef_runner configured — CGAL_DIR: ${CGAL_DIR}")
