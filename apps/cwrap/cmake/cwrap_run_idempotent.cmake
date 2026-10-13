# This script checks that a second rewrap changes nothing and `--check` agrees with the output. It
# rewraps cwrap's own sources, tests and fixtures, and the sources of the libraries that cwrap links:
# `CWRAP_LIBS_DIR` names `libs/src/`, which holds one directory for each library, and
# `CWRAP_LIBRARIES` the libraries, separated by `|`, because a `;` would split the argument.

foreach(required IN ITEMS CWRAP_EXECUTABLE CWRAP_SOURCE_DIR CWRAP_LIBS_DIR CWRAP_LIBRARIES
                         CWRAP_SCRATCH_DIR
)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${CWRAP_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${CWRAP_SCRATCH_DIR}")

# Collect the inputs when the test runs. Runtime lists are not build inputs. Source and fixture
# changes do not require CMake to run again.
file(GLOB_RECURSE source_inputs "${CWRAP_SOURCE_DIR}/src/*.c" "${CWRAP_SOURCE_DIR}/src/*.h")
file(GLOB test_source_inputs "${CWRAP_SOURCE_DIR}/tests/*.c" "${CWRAP_SOURCE_DIR}/tests/*.h")
file(GLOB fixture_inputs "${CWRAP_SOURCE_DIR}/tests/fixtures/*.c"
     "${CWRAP_SOURCE_DIR}/tests/expected/*.c"
)
string(REPLACE "|" ";" libraries "${CWRAP_LIBRARIES}")
set(library_inputs "")
foreach(library IN LISTS libraries)
  file(GLOB_RECURSE library_sources "${CWRAP_LIBS_DIR}/${library}/*.c"
       "${CWRAP_LIBS_DIR}/${library}/*.h"
  )
  if(NOT library_sources)
    message(FATAL_ERROR "library '${library}' has no sources in '${CWRAP_LIBS_DIR}'")
  endif()
  list(APPEND library_inputs ${library_sources})
endforeach()
set(inputs ${fixture_inputs} ${source_inputs} ${test_source_inputs} ${library_inputs})
list(SORT inputs)

set(once_file "${CWRAP_SCRATCH_DIR}/once.c")
set(twice_file "${CWRAP_SCRATCH_DIR}/twice.c")
foreach(width IN ITEMS 20 40 60 100)
  foreach(input IN LISTS inputs)
    execute_process(
      COMMAND "${CWRAP_EXECUTABLE}" -w "${width}" "${input}"
      OUTPUT_FILE "${once_file}"
      RESULT_VARIABLE first_result
      ERROR_VARIABLE first_stderr
    )
    if(NOT first_result EQUAL 0)
      message(
        FATAL_ERROR "first rewrap failed with exit status ${first_result} for '${input}' "
                    "at width ${width}:\n${first_stderr}"
      )
    endif()

    execute_process(
      COMMAND "${CWRAP_EXECUTABLE}" -w "${width}" "${once_file}"
      OUTPUT_FILE "${twice_file}"
      RESULT_VARIABLE second_result
      ERROR_VARIABLE second_stderr
    )
    if(NOT second_result EQUAL 0)
      message(
        FATAL_ERROR "second rewrap failed with exit status ${second_result} for '${input}' "
                    "at width ${width}:\n${second_stderr}"
      )
    endif()

    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E compare_files "${once_file}" "${twice_file}"
      RESULT_VARIABLE compare_result
      OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT compare_result EQUAL 0)
      message(FATAL_ERROR "not idempotent: '${input}' at width ${width}")
    endif()

    execute_process(
      COMMAND "${CWRAP_EXECUTABLE}" --check -w "${width}" "${once_file}"
      OUTPUT_QUIET ERROR_QUIET
      RESULT_VARIABLE check_result
    )
    if(NOT check_result EQUAL 0)
      message(FATAL_ERROR "--check disagrees with own output: '${input}' at width ${width}")
    endif()
  endforeach()
endforeach()
