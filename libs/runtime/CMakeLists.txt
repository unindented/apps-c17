# `runtime`: the host interface `os.h`, the out-of-memory report, the worker pool and the directory
# walk, with `os_posix`, the POSIX implementation of `os.h`.

add_library(runtime STATIC)
target_sources(runtime PRIVATE memory.c pool.c walk.c memory.h os.h pool.h walk.h)
libs_add_include_root(runtime runtime)
target_link_libraries(runtime PUBLIC core shared PRIVATE apps_build_project Threads::Threads)

# The POSIX implementation of `os.h` stays out of `runtime`, so each executable chooses its host when
# it is linked: an app's executable and every unit test registered with `OS posix` link this one,
# and the tests registered with `OS memory` link `os_memory`. It is an object library because
# `runtime` and an app's library refer to it, and a static archive listed before its user would be
# skipped by a single-pass linker.
add_library(os_posix OBJECT)
target_sources(os_posix PRIVATE fs.c fs.h)
libs_add_include_root(os_posix runtime)
target_link_libraries(os_posix PUBLIC runtime PRIVATE apps_build_project)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/fs.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/fs.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/memory.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/memory.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/os.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/pool.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/pool.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_fs.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_memory.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_os.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_pool.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_walk.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/walk.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/walk.h"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(runtime)
apps_enable_project_analysis(os_posix)

if(NOT libs_build_testing)
  return()
endif()

# `fs` is the POSIX host itself, and `pool` reaches no host.
foreach(module IN ITEMS fs pool)
  libs_add_unit_test(runtime ${module} OS posix LIBRARIES testing)
endforeach()
libs_add_unit_test(runtime memory OS memory LIBRARIES testing)
libs_add_unit_test(runtime walk OS memory LIBRARIES testing)
# The contract tests of `os.h`: one source, run against each host.
libs_add_unit_test(runtime os NAME os_posix OS posix LIBRARIES testing)
libs_add_unit_test(runtime os NAME os_memory OS memory LIBRARIES testing)
