# `testing`: the support that every unit test links: test arenas and logs, log and text checks, the
# out-of-memory check, a seeded random generator, fixture files, the working directory, environment
# variables and stream capture. It also defines the `os_oom` of the unit tests.
#
# It is an object library rather than a static one so that its `os_oom` is always linked in: a
# static archive member would be pulled only by a reference that precedes it on the link line, and
# the reference comes from the arena in `shared`, which follows it.

add_library(testing OBJECT)
target_sources(testing PRIVATE test_support.c test_support.h)
libs_add_include_root(testing testing)
target_link_libraries(testing PUBLIC core shared PRIVATE apps_build_tests vendor_acutest)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/test_support.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_support.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_test_support.c"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_test_analysis(testing)

if(NOT libs_build_testing)
  return()
endif()

libs_add_unit_test(testing test_support SOURCE test_test_support.c)
