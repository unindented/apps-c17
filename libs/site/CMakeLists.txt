# `site`: the output manifest, the build job, output paths and the output writer, which the static
# site and gallery generators share.

add_library(site STATIC)
target_sources(
  site PRIVATE job.c manifest.c output_path.c output_writer.c job.h manifest.h output_path.h
               output_writer.h
)
libs_add_include_root(site site)
target_link_libraries(site PUBLIC core runtime shared PRIVATE apps_build_project)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/job.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/job.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/manifest.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/manifest.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_path.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_path.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_writer.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_writer.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_job.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_manifest.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_output_path.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_output_writer.c"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(site)

if(NOT libs_build_testing)
  return()
endif()

foreach(module IN ITEMS manifest output_path)
  libs_add_unit_test(site ${module} OS posix LIBRARIES testing)
endforeach()
# These read and write only through `runtime/os.h`, so they run against the in-memory host.
foreach(module IN ITEMS job output_writer)
  libs_add_unit_test(site ${module} OS memory LIBRARIES testing)
endforeach()
