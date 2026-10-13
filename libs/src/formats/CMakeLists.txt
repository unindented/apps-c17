# `formats`: HTML escaping, and TOML parsing with key checks, text checks and dates.

add_library(formats STATIC)
target_sources(formats PRIVATE html.c toml.c html.h toml.h)
libs_add_include_root(formats formats)
target_link_libraries(formats PUBLIC core vendor_tomlc17 PRIVATE apps_build_project)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/html.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/html.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_html.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_toml.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/toml.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/toml.h"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(formats)

if(NOT libs_build_testing)
  return()
endif()

foreach(module IN ITEMS html toml)
  libs_add_unit_test(formats ${module} LIBRARIES testing)
endforeach()
