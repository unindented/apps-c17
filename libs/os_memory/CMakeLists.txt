# `os_memory`: the in-memory implementation of `runtime/os.h`, which a unit test registered with
# `OS memory` links in place of `os_posix`, so it builds its tree without touching the disk. It is
# an object library for the same reason as `os_posix`.

add_library(os_memory OBJECT)
target_sources(os_memory PRIVATE os_memory.c os_memory.h)
libs_add_include_root(os_memory os_memory)
target_link_libraries(
  os_memory PUBLIC runtime PRIVATE apps_build_tests vendor_acutest testing Threads::Threads
)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources "${CMAKE_CURRENT_SOURCE_DIR}/os_memory.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/os_memory.h"
)

if(PROJECT_IS_TOP_LEVEL)
  apps_enable_test_analysis(os_memory)
endif()
