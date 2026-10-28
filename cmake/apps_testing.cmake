# This helper registers a unit test built from `test_<module>.c` in the calling directory.

include_guard(GLOBAL)

# The calling app names its targets after its project name, `<app>`: the test links `<app>_app`, the
# `testing` library, `<app>_test_support` when the app defines it, and a host, and is registered as
# `<app>.<module>`. The host is the library's `os_<OS>`.
#
# `OS` names the implementation of the host interface the test links: `posix` (the default) or
# `memory` for an in-memory host. Every test registered without `OS memory` links the POSIX host:
# the tests that need the real host, such as fram's process and video tests and surge's tests that
# read git repositories; the suites written before the in-memory host, which stay on it until they
# are converted; and the tests that reach no host. New tests use the in-memory host. A test that
# links the in-memory host is compiled with `<APP>_TEST_OS_MEMORY` set to 1.
function(apps_add_unit_test module)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "OS" "")
  if(NOT arg_OS)
    set(arg_OS posix)
  endif()
  set(app "${PROJECT_NAME}")
  string(TOUPPER "${app}" app_upper)
  set(target "${app}_unit_${module}")
  add_executable(${target} "test_${module}.c")
  target_link_libraries(
    ${target} PRIVATE apps_build_tests vendor_acutest testing ${app}_app os_${arg_OS}
  )
  # An app can add test helpers of its own in `<app>_test_support`, which its `tests/` directory
  # defines after the source directories register their tests, so the link is resolved at generate
  # time.
  target_link_libraries(${target} PRIVATE "$<TARGET_NAME_IF_EXISTS:${app}_test_support>")
  if(arg_OS STREQUAL "memory")
    target_compile_definitions(${target} PRIVATE ${app_upper}_TEST_OS_MEMORY=1)
  endif()

  add_test(NAME "${app}.${module}" COMMAND ${target} WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  # A unit test that hangs fails after this many seconds instead of stopping the whole run. That is
  # several times the slowest unit test in an 8-job ASan run on an Apple M1 Max, which took under
  # 10 seconds. ThreadSanitizer is slower: the slowest unit test took about 18 seconds under it on
  # an Apple M1 Max, and up to 61 seconds on a GitHub macOS runner, so a TSan build waits longer.
  set(timeout 60)
  if(APPS_SANITIZER STREQUAL "thread")
    set(timeout 300)
  endif()
  set_tests_properties(
    "${app}.${module}"
    PROPERTIES LABELS "${app};unit"
               TIMEOUT ${timeout}
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )

  if(PROJECT_IS_TOP_LEVEL)
    apps_enable_test_analysis(${target})
  endif()
endfunction()
