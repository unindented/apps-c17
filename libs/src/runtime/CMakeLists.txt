# `runtime`: the worker pool, the jobs of a build phase that run on it, and the directory walk, which
# reads directories through `os/os.h`.

add_library(runtime STATIC)
target_sources(runtime PRIVATE job.c pool.c walk.c job.h pool.h walk.h)
libs_add_include_root(runtime runtime)
target_link_libraries(runtime PUBLIC core os PRIVATE apps_build_project Threads::Threads)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/job.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/job.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/pool.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/pool.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_job.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_pool.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_walk.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_walk_posix.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/walk.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/walk.h"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(runtime)

if(NOT libs_build_testing)
  return()
endif()

# `pool` reaches no host, so it links the POSIX one.
libs_add_unit_test(runtime pool OS posix LIBRARIES testing)
libs_add_unit_test(runtime walk OS memory LIBRARIES testing)
# A job prints its progress only through `os/os.h`, so it runs against the in-memory host.
libs_add_unit_test(runtime job OS memory LIBRARIES testing)
libs_add_unit_test(runtime walk_posix OS posix LIBRARIES testing)
