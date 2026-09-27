# The golden test suite uses several modules. Keep it in the top-level `tests/` directory.

# The in-memory implementation of `src/runtime/os.h`, which a unit test registered with `OS memory`
# links in place of the POSIX one. It is an object library for the same reason as the POSIX one.
add_library(sosig_os_memory OBJECT os_memory.c)
target_include_directories(sosig_os_memory PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>")
target_link_libraries(sosig_os_memory PRIVATE apps_build_tests vendor_acutest testing sosig_app)
if(PROJECT_IS_TOP_LEVEL)
  apps_enable_test_analysis(sosig_os_memory)
endif()

set(sosig_fixture_sites site_file_permalink site_index_permalink site_nested_sections)

function(sosig_add_golden_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "SITE;EXPECTED" "")
  add_test(
    NAME "${name}"
    COMMAND
      "${CMAKE_COMMAND}" "-DSOSIG_EXECUTABLE=$<TARGET_FILE:sosig>" "-DSOSIG_SITE_DIR=${arg_SITE}"
      "-DSOSIG_EXPECTED_DIR=${arg_EXPECTED}"
      "-DSOSIG_SCRATCH_DIR=${PROJECT_BINARY_DIR}/golden/$<CONFIG>/${name}" -P
      "${PROJECT_SOURCE_DIR}/cmake/sosig_run_golden.cmake"
  )
  set_tests_properties(
    "${name}"
    PROPERTIES LABELS "sosig;golden"
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )
endfunction()

foreach(site IN LISTS sosig_fixture_sites)
  set(site_dir "${CMAKE_CURRENT_LIST_DIR}/fixtures/${site}")
  if(NOT IS_DIRECTORY "${site_dir}")
    message(FATAL_ERROR "fixture site '${site}' is listed but '${site_dir}' does not exist")
  endif()

  sosig_add_golden_test(
    "sosig.golden.${site}"
    SITE "${site_dir}"
    EXPECTED "${CMAKE_CURRENT_LIST_DIR}/expected/${site}"
  )
endforeach()
