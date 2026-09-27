# The golden and idempotence test suites use several modules. Keep them in the top-level `tests/`
# directory.

# The in-memory implementation of `src/runtime/os.h`, which a unit test registered with `OS memory`
# links in place of the POSIX one. It is an object library for the same reason as the POSIX one.
add_library(cwrap_os_memory OBJECT os_memory.c)
target_include_directories(cwrap_os_memory PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>")
target_link_libraries(cwrap_os_memory PRIVATE apps_build_tests vendor_acutest testing cwrap_app)
if(PROJECT_IS_TOP_LEVEL)
  apps_enable_test_analysis(cwrap_os_memory)
endif()

# Seeded random tests over the rewrap.
apps_add_unit_test(random)

add_test(
  NAME cwrap.golden
  COMMAND
    "${CMAKE_COMMAND}" "-DCWRAP_EXECUTABLE=$<TARGET_FILE:cwrap>"
    "-DCWRAP_FIXTURE_DIR=${CMAKE_CURRENT_LIST_DIR}/fixtures"
    "-DCWRAP_EXPECTED_DIR=${CMAKE_CURRENT_LIST_DIR}/expected"
    "-DCWRAP_SCRATCH_DIR=${PROJECT_BINARY_DIR}/golden/$<CONFIG>" -P
    "${PROJECT_SOURCE_DIR}/cmake/cwrap_run_golden.cmake"
)
set_tests_properties(
  cwrap.golden
  PROPERTIES LABELS "cwrap;golden"
             ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
)

# The idempotence test also rewraps the sources of every library that cwrap links, which
# `APPS_LIBRARIES` lists, separated by `|` because a `;` would split the argument.
string(REPLACE ";" "|" cwrap_idempotent_libraries "${APPS_LIBRARIES}")
add_test(
  NAME cwrap.idempotent
  COMMAND
    "${CMAKE_COMMAND}" "-DCWRAP_EXECUTABLE=$<TARGET_FILE:cwrap>"
    "-DCWRAP_SOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DCWRAP_LIBS_DIR=${PROJECT_SOURCE_DIR}/../../libs"
    "-DCWRAP_LIBRARIES=${cwrap_idempotent_libraries}"
    "-DCWRAP_SCRATCH_DIR=${PROJECT_BINARY_DIR}/idempotent/$<CONFIG>" -P
    "${PROJECT_SOURCE_DIR}/cmake/cwrap_run_idempotent.cmake"
)
set_tests_properties(
  cwrap.idempotent
  PROPERTIES LABELS "cwrap;idempotent"
             ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
)
