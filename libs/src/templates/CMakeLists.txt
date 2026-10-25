# `templates`: the template set of a build, which compiles every page template and partial once,
# and its renders, over data that an app gives through callbacks.

add_library(templates STATIC)
target_sources(templates PRIVATE template_set.c template_set.h)
libs_add_include_root(templates templates)
target_link_libraries(templates PUBLIC core os PRIVATE apps_build_project formats vendor_mustache)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/template_set.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/template_set.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_template_set.c"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(templates)

if(NOT libs_build_testing)
  return()
endif()

# A set reads its templates only through `os/os.h`, so the tests run against the in-memory host.
libs_add_unit_test(templates template_set OS memory LIBRARIES testing)
