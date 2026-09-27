cmake_minimum_required(VERSION 3.25...4.0)

# Set this target before `project()` because compiler checks use it. Builders can raise the value.
set(CMAKE_OSX_DEPLOYMENT_TARGET
    "13.0"
    CACHE STRING "Minimum macOS version the build targets"
)

# This project holds the libraries that the apps share, one target and one include root for each.
# An app adds it with `add_subdirectory(... EXCLUDE_FROM_ALL SYSTEM)`, so a library builds only when
# the app links it, with the app's build profiles and vendored targets. Configured on its own, the
# project defines the vendored targets it needs and builds every library's tests, analysis and lint.
project(
  apps_libs
  DESCRIPTION "The libraries that the C17 apps share"
  HOMEPAGE_URL "https://github.com/unindented/apps-c17"
  LANGUAGES C
)

if(PROJECT_IS_TOP_LEVEL AND "${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
  message(FATAL_ERROR "in-source builds are unsupported; use a preset or '-B build/<name>'")
endif()

# Only the top-level project builds the libraries' tests. An app that adds this project never does,
# whatever its own `APPS_BUILD_TESTING` is.
set(libs_build_testing FALSE)
if(PROJECT_IS_TOP_LEVEL)
  if(NOT DEFINED APPS_BUILD_TESTING)
    set(APPS_BUILD_TESTING TRUE)
  endif()
  if(APPS_BUILD_TESTING)
    set(libs_build_testing TRUE)
    enable_testing()
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

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/../cmake")

include(apps_build_profiles)

if(PROJECT_IS_TOP_LEVEL)
  include(apps_quality)
  include(vendor.cmake)
endif()

# Registers a unit test of `library`, built from `SOURCE` (by default `test_<module>.c` in the
# calling directory), as the test `<library>.<module>`. The test links the library and
# `LIBRARIES`.
function(libs_add_unit_test library module)
  cmake_parse_arguments(PARSE_ARGV 2 arg "" "SOURCE" "LIBRARIES")
  if(NOT arg_SOURCE)
    set(arg_SOURCE "test_${module}.c")
  endif()
  set(target "${library}_unit_${module}")
  add_executable(${target} "${arg_SOURCE}")
  target_link_libraries(
    ${target} PRIVATE apps_build_tests vendor_acutest ${library} ${arg_LIBRARIES}
  )

  add_test(NAME "${library}.${module}" COMMAND ${target} WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  # A unit test that hangs fails after this many seconds instead of stopping the whole run. The apps
  # use the same limits, which `apps_add_unit_test` explains.
  set(timeout 60)
  if(APPS_SANITIZER STREQUAL "thread")
    set(timeout 300)
  endif()
  set_tests_properties(
    "${library}.${module}"
    PROPERTIES LABELS "${library};unit"
               TIMEOUT ${timeout}
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )
  apps_enable_test_analysis(${target})
endfunction()

# Gives `target` the include root of `library`: a directory in the build tree that holds one link,
# named for the library, to the library's source directory. `#include "<library>/<module>.h"` then
# finds the library's headers through that root and no other file of `libs/`, so a library can
# have the same layer name as a directory of an app.
function(libs_add_include_root target library)
  set(root "${PROJECT_BINARY_DIR}/include/${library}")
  if(NOT EXISTS "${root}/${library}")
    file(MAKE_DIRECTORY "${root}")
    file(CREATE_LINK "${PROJECT_SOURCE_DIR}/${library}" "${root}/${library}" SYMBOLIC)
  endif()
  target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${root}>")
endfunction()

# Each library appends its own sources to `apps_libs_lint_sources`.
add_subdirectory(shared)
add_subdirectory(core)
add_subdirectory(testing)

if(PROJECT_IS_TOP_LEVEL)
  get_property(libs_lint_sources GLOBAL PROPERTY apps_libs_lint_sources)
  apps_add_lint_targets(SOURCES ${libs_lint_sources})
endif()
