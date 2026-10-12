# `output`: the output manifest, the output-path checks and the output writer, which make and check
# every file a build writes under its output directory.

add_library(output STATIC)
target_sources(
  output PRIVATE manifest.c output_path.c output_writer.c manifest.h output_path.h output_writer.h
)
libs_add_include_root(output output)
target_link_libraries(output PUBLIC core os PRIVATE apps_build_project)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/manifest.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/manifest.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_path.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_path.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_writer.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_writer.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_manifest.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_output_path.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_output_writer.c"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(output)

if(NOT libs_build_testing)
  return()
endif()

foreach(module IN ITEMS manifest output_path)
  libs_add_unit_test(output ${module} OS posix LIBRARIES testing)
endforeach()
# It writes only through `os/os.h`, so it runs against the in-memory host.
libs_add_unit_test(output output_writer OS memory LIBRARIES testing)
