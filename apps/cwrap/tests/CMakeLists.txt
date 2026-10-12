# The golden and idempotence test suites use several modules. Keep them in the top-level `tests/`
# directory.

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
