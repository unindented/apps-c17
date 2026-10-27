# `formats`: HTML escaping, front matter fences, TOML parsing with key checks, text checks and dates,
# and the escaping of a tab-separated field.

add_library(formats STATIC)
target_sources(
  formats PRIVATE frontmatter.c html.c toml.c toml_config.c tsv.c frontmatter.h html.h rfc3339.h toml.h tsv.h
)
libs_add_include_root(formats formats)
target_link_libraries(formats PUBLIC core os vendor_tomlc17 PRIVATE apps_build_project)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/frontmatter.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/frontmatter.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/html.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/html.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/rfc3339.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_frontmatter.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_html.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_toml.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_tsv.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/toml.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/toml.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/toml_config.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/tsv.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/tsv.h"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(formats)

if(NOT libs_build_testing)
  return()
endif()

foreach(module IN ITEMS frontmatter html tsv)
  libs_add_unit_test(formats ${module} LIBRARIES testing)
endforeach()
# The config read goes through `os/os.h`, so the TOML tests run against the in-memory host.
libs_add_unit_test(formats toml OS memory LIBRARIES testing)
