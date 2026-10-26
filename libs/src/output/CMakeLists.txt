# `output`: the output manifest, the output-path checks, the output writer and the prune, which make
# and check every file a build writes under its output directory.

add_library(output STATIC)
target_sources(
  output
  PRIVATE build_cache_file.c
          manifest.c
          output_path.c
          output_pruner.c
          output_writer.c
          build_cache_file.h
          manifest.h
          output_path.h
          output_pruner.h
          output_writer.h
)
libs_add_include_root(output output)
target_link_libraries(output PUBLIC core os PRIVATE apps_build_project)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/build_cache_file.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/build_cache_file.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/manifest.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/manifest.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_path.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_path.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_pruner.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_pruner.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_writer.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/output_writer.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_build_cache_file.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_manifest.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_output_path.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_output_pruner.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_output_pruner_case.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_output_writer.c"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(output)

if(NOT libs_build_testing)
  return()
endif()

# A case-insensitive filesystem is a fact of the real host, so the prune's case is on the POSIX one.
foreach(module IN ITEMS manifest output_path output_pruner_case)
  libs_add_unit_test(output ${module} OS posix LIBRARIES testing)
endforeach()
# These read and write only through `os/os.h`, so they run against the in-memory host.
foreach(module IN ITEMS build_cache_file output_pruner output_writer)
  libs_add_unit_test(output ${module} OS memory LIBRARIES testing)
endforeach()
