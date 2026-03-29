cmake_minimum_required(VERSION 3.20)
project(carve_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Carve — built from source via add_subdirectory.
# CARVE_SOURCE_DIR is set by build.py via -DCARVE_SOURCE_DIR=<path>.
# ---------------------------------------------------------------------------
if(NOT DEFINED CARVE_SOURCE_DIR OR NOT EXISTS "${CARVE_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "CARVE_SOURCE_DIR must be set to the Carve source tree.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()

# Disable Carve extras we don't need.
set(CARVE_WITH_GUI           OFF CACHE BOOL "" FORCE)
set(CARVE_BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING            OFF CACHE BOOL "" FORCE)
set(CARVE_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(CARVE_BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
# CARVE_BOOST_COLLECTIONS defaults to ON, which forces Carve's bundled boost_impl.hpp.
# That file injects std::unordered_map into namespace std, conflicting with C++17 MSVC's
# already-defined std::unordered_map.  Setting it OFF lets Carve detect and use
# std::unordered_map directly (Carve's data_type usages are patched to mapped_type).
set(CARVE_BOOST_COLLECTIONS  OFF CACHE BOOL "" FORCE)

add_subdirectory("${CARVE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/carve_build" EXCLUDE_FROM_ALL)

# Dependencies — provided by vcpkg (see build.py)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(carve_runner
  src/main.cpp
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/_common/cpp/include")

target_include_directories(carve_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
  ${CARVE_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/carve_build/include
)

# Carve's carve.hpp selects the config header based on CMAKE_BUILD / _MSC_VER etc.
# Without CMAKE_BUILD, MSVC falls through to vcpp_config.h which hardcodes
# HAVE_BOOST_UNORDERED_COLLECTIONS.  Define CMAKE_BUILD so it picks the generated
# config.h (which has HAVE_STD_UNORDERED_COLLECTIONS, consistent with the library).
target_compile_definitions(carve_runner PRIVATE CMAKE_BUILD)

target_link_libraries(carve_runner PRIVATE
  carve
  nlohmann_json::nlohmann_json
)

message(STATUS "carve_runner configured — CARVE_SOURCE_DIR: ${CARVE_SOURCE_DIR}")
