cmake_minimum_required(VERSION 3.25...4.0)

# Set this target before `project()` because compiler checks use it. Builders can raise the value.
set(CMAKE_OSX_DEPLOYMENT_TARGET
    "13.0"
    CACHE STRING "Minimum macOS version the build targets"
)

project(
  fram
  VERSION 0.5.0
  DESCRIPTION "A small static photo and video gallery generator written in C17"
  HOMEPAGE_URL "https://github.com/unindented/apps-c17"
  LANGUAGES C
)

if(PROJECT_IS_TOP_LEVEL AND "${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
  message(FATAL_ERROR "in-source builds are unsupported; use a preset or '-B build/<name>'")
endif()

if(NOT DEFINED APPS_BUILD_TESTING)
  set(APPS_BUILD_TESTING "${PROJECT_IS_TOP_LEVEL}")
endif()

if(APPS_BUILD_TESTING)
  if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
  else()
    message(STATUS "fram tests are enabled as a child; the parent must call 'enable_testing()' at its top level")
  endif()
endif()

if(NOT DEFINED CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 17)
endif()
if(NOT DEFINED CMAKE_C_STANDARD_REQUIRED)
  set(CMAKE_C_STANDARD_REQUIRED TRUE)
endif()
if(NOT DEFINED CMAKE_C_EXTENSIONS)
  set(CMAKE_C_EXTENSIONS FALSE)
endif()

set(CMAKE_LINK_LIBRARIES_ONLY_TARGETS TRUE)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
endif()

# The root `cmake/` directory holds the modules that every project shares, and the app's own
# `cmake/` directory holds the modules of this app.
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/../../cmake" "${PROJECT_SOURCE_DIR}/cmake")

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

include(apps_build_profiles)

if(PROJECT_IS_TOP_LEVEL)
  include(apps_quality)
  include(fram_lint)
endif()
if(APPS_BUILD_TESTING)
  include(apps_testing)
endif()

include(vendor.cmake)
# The libraries that this app and its tests link. `apps_quality` gives their include roots to
# cppcheck.
set(APPS_LIBRARIES core shared testing)
# The libraries build only when a target of this app links them, with this app's build profiles.
# `SYSTEM` keeps this app's analysis to its own sources.
add_subdirectory(../../libs ${CMAKE_BINARY_DIR}/libs EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory(src)

if(APPS_BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(PROJECT_IS_TOP_LEVEL)
  include(apps_packaging)
endif()
