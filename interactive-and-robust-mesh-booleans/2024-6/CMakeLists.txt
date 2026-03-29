cmake_minimum_required(VERSION 3.21)
project(iarmb_runner LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# IARMB_SOURCE_DIR — set by build.py via -DIARMB_SOURCE_DIR=<path>
# ---------------------------------------------------------------------------
if(NOT DEFINED IARMB_SOURCE_DIR OR NOT EXISTS "${IARMB_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "IARMB_SOURCE_DIR must be set to the iarmb source tree.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()

# ---------------------------------------------------------------------------
# oneTBB — vendored in-tree; build from source.
# Disable tests and examples to keep build times reasonable.
# ---------------------------------------------------------------------------
set(TBB_TEST     OFF CACHE BOOL "" FORCE)
set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${IARMB_SOURCE_DIR}/arrangements/external/oneTBB"
                 "${CMAKE_BINARY_DIR}/oneTBB_build"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# cinolib — vendored in-tree; header-only (no OpenGL/GLFW needed).
# ---------------------------------------------------------------------------
set(cinolib_DIR "${IARMB_SOURCE_DIR}/arrangements/external/Cinolib")
set(CINOLIB_USES_OPENGL_GLFW_IMGUI   OFF CACHE BOOL "" FORCE)
set(CINOLIB_USES_SHEWCHUK_PREDICATES ON  CACHE BOOL "" FORCE)
find_package(cinolib REQUIRED)

# Dependencies — provided by vcpkg (see build.py)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runner executable
#
# The upstream library uses a "header-includes-cpp" unity build pattern:
# booleans.h includes booleans.cpp, which transitively includes all other
# implementation files. Only our src/main.cpp needs to be compiled directly.
# ---------------------------------------------------------------------------
add_executable(iarmb_runner src/main.cpp)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/runners/_common/cpp/include")

target_include_directories(iarmb_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
  # iarmb library headers (mirrors their own CMakeLists include paths)
  ${IARMB_SOURCE_DIR}
  ${IARMB_SOURCE_DIR}/code
  ${IARMB_SOURCE_DIR}/arrangements/code
  ${IARMB_SOURCE_DIR}/arrangements/external/Indirect_Predicates/include
  ${IARMB_SOURCE_DIR}/arrangements/external/abseil-cpp
  ${IARMB_SOURCE_DIR}/arrangements/external/oneTBB/include
)

target_link_libraries(iarmb_runner PRIVATE
  cinolib
  tbb
  nlohmann_json::nlohmann_json
)

target_compile_definitions(iarmb_runner PRIVATE TBB_PARALLEL=1)

# The vendored oneTBB subdirectory may place its shared library in the build
# tree rather than next to the executable.  Copy it explicitly so the runner
# is self-contained in bin/.
if(WIN32)
  # Windows: copy the DLL; the loader searches the EXE directory automatically.
  add_custom_command(TARGET iarmb_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:tbb>
      $<TARGET_FILE_DIR:iarmb_runner>
    COMMENT "Copying tbb DLL to output directory"
  )
else()
  # Linux/macOS: copy the .so/.dylib using its SONAME (e.g. libtbb.so.12) so
  # the dynamic linker resolves it, and embed $ORIGIN in the RPATH so the
  # executable finds it in its own directory without LD_LIBRARY_PATH.
  add_custom_command(TARGET iarmb_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:tbb>
      "$<TARGET_FILE_DIR:iarmb_runner>/$<TARGET_SONAME_FILE_NAME:tbb>"
    COMMENT "Copying TBB shared library to output directory"
  )
  set_target_properties(iarmb_runner PROPERTIES
    BUILD_RPATH   "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags — mirrors the upstream CMakeLists.txt
# ---------------------------------------------------------------------------
set(ENABLE_AVX2 ON)
set(ENABLE_SSE2 ON)

if(MSVC)
  # Grant IEEE 754 compliance (required for exact predicates)
  target_compile_options(iarmb_runner PRIVATE /fp:strict)
  # Use intrinsic functions
  target_compile_options(iarmb_runner PRIVATE /Oi)
  # SIMD
  if(ENABLE_AVX2)
    target_compile_options(iarmb_runner PRIVATE /arch:AVX2)
  elseif(ENABLE_SSE2)
    target_compile_options(iarmb_runner PRIVATE /arch:SSE2)
  endif()
  # Reserve enough stack for deep recursion
  target_link_options(iarmb_runner PRIVATE /STACK:8421376)
  # Silence CRT security warnings
  target_compile_definitions(iarmb_runner PRIVATE _CRT_SECURE_NO_WARNINGS)
  # Suppress Windows min/max macros (conflict with std::numeric_limits)
  target_compile_definitions(iarmb_runner PRIVATE NOMINMAX)
  # Exclude rarely-used OLE headers that typedef 'byte' as unsigned char,
  # which conflicts with std::byte in C++17
  target_compile_definitions(iarmb_runner PRIVATE WIN32_LEAN_AND_MEAN)
else()
  # Grant IEEE 754 compliance (required for exact predicates)
  target_compile_options(iarmb_runner PRIVATE -frounding-math)
  # SIMD
  if(ENABLE_AVX2)
    target_compile_options(iarmb_runner PRIVATE -mavx2)
  elseif(ENABLE_SSE2)
    target_compile_options(iarmb_runner PRIVATE -msse2)
  endif()
  # Reserve enough stack for deep recursion (Linux only)
  if(UNIX AND NOT APPLE)
    target_link_options(iarmb_runner PRIVATE -Wl,-z,stacksize=8421376)
  endif()
endif()

message(STATUS "iarmb_runner configured — IARMB_SOURCE_DIR: ${IARMB_SOURCE_DIR}")
