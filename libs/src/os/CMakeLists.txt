# `os`: the host interface `os.h` and the out-of-memory report, with `os_posix`, the POSIX
# implementation of `os.h`, and `os_memory`, the in-memory one that unit tests link.

add_library(os STATIC)
target_sources(os PRIVATE oom.c oom.h os.h os_memory.h os_posix.h)
libs_add_include_root(os os)
target_link_libraries(os PUBLIC core PRIVATE apps_build_project)

# The implementations of `os.h` stay out of `os`, so each executable chooses its host when it is
# linked: an app's executable and every unit test registered with `OS posix` link `os_posix`, and
# the tests registered with `OS memory` link `os_memory`, so they build their tree without touching
# the disk. They are object libraries because `os` and an app's library refer to them, and a static
# archive listed before its user would be skipped by a single-pass linker.
add_library(os_posix OBJECT)
target_sources(os_posix PRIVATE os_posix.c)
target_link_libraries(os_posix PUBLIC os PRIVATE apps_build_project)

add_library(os_memory OBJECT)
target_sources(os_memory PRIVATE os_memory.c)
target_link_libraries(
  os_memory PUBLIC os PRIVATE apps_build_tests vendor_acutest testing Threads::Threads
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(os)
apps_enable_project_analysis(os_posix)
apps_enable_test_analysis(os_memory)

if(NOT libs_build_testing)
  return()
endif()

libs_add_unit_test(os os_posix OS posix LIBRARIES testing)
libs_add_unit_test(os oom OS memory LIBRARIES testing)
# The contract tests of `os.h`: one source, run against each host.
libs_add_unit_test(os os NAME contract_posix OS posix LIBRARIES testing)
libs_add_unit_test(os os NAME contract_memory OS memory LIBRARIES testing)
