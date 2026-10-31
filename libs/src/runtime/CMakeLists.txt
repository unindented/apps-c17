# `runtime`: the run of a build in its memory, the worker pool, the jobs of a build phase that run on
# it, the check of the directories a build reads, the directory walk, which reads directories
# through `os/os.h`, and the status lines of a verbose command.

add_library(runtime STATIC)
target_sources(
  runtime
  PRIVATE build_run.c
          job.c
          pool.c
          read_root.c
          verbose.c
          walk.c
          build_run.h
          job.h
          pool.h
          read_root.h
          verbose.h
          walk.h
)
libs_add_include_root(runtime runtime)
target_link_libraries(runtime PUBLIC core os PRIVATE apps_build_project Threads::Threads)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(runtime)

if(NOT libs_build_testing)
  return()
endif()

# `pool` reaches no host, so it links the POSIX one.
libs_add_unit_test(runtime pool OS posix LIBRARIES testing)
libs_add_unit_test(runtime build_run OS memory LIBRARIES testing)
libs_add_unit_test(runtime read_root OS memory LIBRARIES testing)
libs_add_unit_test(runtime verbose OS memory LIBRARIES testing)
libs_add_unit_test(runtime walk OS memory LIBRARIES testing)
# A job prints its progress only through `os/os.h`, so it runs against the in-memory host.
libs_add_unit_test(runtime job OS memory LIBRARIES testing)
libs_add_unit_test(runtime walk_posix OS posix LIBRARIES testing)
