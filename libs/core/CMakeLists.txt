# `core`: ASCII classification, error text, number parsing, paths, path lists, text helpers and the
# exit codes of the apps.

add_library(core STATIC)
target_sources(
  core PRIVATE error.c parse.c path.c path_list.c text.c ascii.h error.h exit_code.h parse.h path.h
               path_list.h text.h
)
libs_add_include_root(core core)
target_link_libraries(core PUBLIC shared PRIVATE apps_build_project)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/ascii.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/error.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/error.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/exit_code.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/parse.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/parse.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/path.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/path.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/path_list.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/path_list.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_ascii.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_error.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_parse.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_path.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_path_list.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_text.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/text.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/text.h"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(core)

if(NOT libs_build_testing)
  return()
endif()

foreach(module IN ITEMS ascii error parse path path_list text)
  libs_add_unit_test(core ${module} LIBRARIES testing)
endforeach()
