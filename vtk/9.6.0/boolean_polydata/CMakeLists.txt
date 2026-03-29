cmake_minimum_required(VERSION 3.20)
project(vtk_boolean_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# VTK — installed from source by build.py.
# VTK_DIR must point to the VTK CMake config directory.
# ---------------------------------------------------------------------------
if(NOT DEFINED VTK_DIR OR NOT EXISTS "${VTK_DIR}")
  message(FATAL_ERROR "VTK_DIR must be set to the VTK CMake config directory.\n"
                       "Run build.py instead of invoking cmake directly.")
endif()

find_package(VTK REQUIRED COMPONENTS
  FiltersGeneral
  FiltersCore
  CommonCore
  CommonDataModel
)

# Dependencies — provided by vcpkg (see build.py)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runner executable
# ---------------------------------------------------------------------------
add_executable(vtk_boolean_runner
  src/main.cpp
)

if(NOT DEFINED PROJECT_ROOT)
  message(FATAL_ERROR "PROJECT_ROOT must be passed by build.py via -DPROJECT_ROOT=<path>")
endif()
set(COMMON_INCLUDE_DIR "${PROJECT_ROOT}/_common/cpp/include")

target_include_directories(vtk_boolean_runner PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${COMMON_INCLUDE_DIR}
)

target_link_libraries(vtk_boolean_runner PRIVATE
  VTK::FiltersGeneral
  VTK::FiltersCore
  VTK::CommonCore
  VTK::CommonDataModel
  nlohmann_json::nlohmann_json
)

# VTK requires module autoinit for factory registration.
vtk_module_autoinit(
  TARGETS vtk_boolean_runner
  MODULES VTK::FiltersGeneral VTK::FiltersCore VTK::CommonCore VTK::CommonDataModel
)

if(UNIX)
  set_target_properties(vtk_boolean_runner PROPERTIES
    BUILD_RPATH  "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()

message(STATUS "vtk_boolean_runner configured — VTK_DIR: ${VTK_DIR}")
