# SPDX-License-Identifier: GPL-2.0-only
#
# Build-only top level for the i1n1-host-efi repository.
cmake_minimum_required(VERSION 3.22)

# Same deployment target as the other i1n1 repositories so C objects agree.
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")
endif()

project(i1n1-host-efi VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(GLOBAL PROPERTY I1N1_REPO_HOST_EFI_LOADED TRUE)
set(I1N1_HOST_EFI_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")

include("${CMAKE_CURRENT_LIST_DIR}/cmake/i1n1_deps.cmake")

i1n1_require(core)
i1n1_require(engine)
i1n1_require(platform)

# The engine is always provided by the i1n1-engine repository in this split;
# kept as a variable because the module CMake files test it.
set(I1N1_WITH_ENGINE ON)

add_subdirectory(src/host/efi)
