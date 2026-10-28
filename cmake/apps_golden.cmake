# The steps that the golden tests of sosig, fram and surge share. A golden script includes this file
# and builds a copied fixture with them: it runs the builds, checks that a later build leaves the
# output as the first left it, checks that a dry run keeps every output, and compares the output
# with the expected tree.

include_guard(GLOBAL)

# Runs `<executable> build <args>` in `WORKING_DIRECTORY`, through `EMULATOR` when it is set, and
# fails the test with the build's output when it exits with anything but `0`. The build's `stdout`
# and `stderr` go to the variables `STDOUT` and `STDERR` name, when given. `LABEL` names the build
# in the failure.
function(apps_golden_run_build)
  cmake_parse_arguments(
    PARSE_ARGV 0 arg "" "EXECUTABLE;WORKING_DIRECTORY;LABEL;STDOUT;STDERR" "EMULATOR;ARGS"
  )
  execute_process(
    COMMAND ${arg_EMULATOR} "${arg_EXECUTABLE}" build ${arg_ARGS}
    WORKING_DIRECTORY "${arg_WORKING_DIRECTORY}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
  )
  if(NOT build_result EQUAL 0)
    message(
      FATAL_ERROR "${arg_LABEL} build failed with exit status ${build_result}:\n"
                  "${build_stdout}${build_stderr}"
    )
  endif()
  if(arg_STDOUT)
    set(${arg_STDOUT} "${build_stdout}" PARENT_SCOPE)
  endif()
  if(arg_STDERR)
    set(${arg_STDERR} "${build_stderr}" PARENT_SCOPE)
  endif()
endfunction()

# Fails the test when a file of `FIRST_DIR`, the output of the first build, differs in `ACTUAL_DIR`
# after a later build, other than one whose relative path matches `IGNORE`, such as the build cache,
# which records the times of its sources. `LABEL` names the later build in the failure.
function(apps_golden_check_unchanged)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "FIRST_DIR;ACTUAL_DIR;LABEL;IGNORE" "")
  file(GLOB_RECURSE first_files RELATIVE "${arg_FIRST_DIR}" "${arg_FIRST_DIR}/*")
  foreach(file IN LISTS first_files)
    if(arg_IGNORE AND file MATCHES "${arg_IGNORE}")
      continue()
    endif()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E compare_files "${arg_FIRST_DIR}/${file}"
              "${arg_ACTUAL_DIR}/${file}"
      RESULT_VARIABLE compare_result
      OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT compare_result EQUAL 0)
      message(FATAL_ERROR "${arg_LABEL} build changed generated output: '${file}'")
    endif()
  endforeach()
endfunction()

# Fails the test when the listing of a dry run, `LISTING`, would change anything after a build:
# every line must keep its output, but for the build cache at `CACHE_PATH`, which every build writes
# again, and an output whose path matches `WRITTEN`, which the app writes on every build, as fram
# renders its pages.
function(apps_golden_check_dry_run_keeps)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "LISTING;CACHE_PATH;WRITTEN" "")
  string(REGEX MATCHALL "[^\n]+" lines "${arg_LISTING}")
  foreach(line IN LISTS lines)
    if(arg_WRITTEN AND line MATCHES "^write\t" AND line MATCHES "${arg_WRITTEN}")
      continue()
    endif()
    if(NOT line MATCHES "^keep\t" AND NOT line STREQUAL "write\t${arg_CACHE_PATH}")
      message(FATAL_ERROR "the dry run after a build would change an output: '${line}'")
    endif()
  endforeach()
endfunction()

# Compares the tree `ACTUAL_DIR` with `EXPECTED_DIR`, leaving out the files of `ACTUAL_DIR` that
# `EXCLUDE` lists. Each side's names are looked up on the other side's filesystem rather than
# compared as strings: a sync tool may rewrite a checked-out name's Unicode normalization, which
# macOS's filesystems then match either way, while Linux keeps git's bytes. A file whose path
# matches `VERSIONED` has `@PROJECT_VERSION@` in its expected copy stand for `VERSION`. A file whose
# path matches `SIZE_ONLY` need only be non-empty, for bytes that vary with a codec's version. Every
# other file is compared byte for byte. `SCRATCH_DIR` holds the expected copy of a versioned file.
function(apps_golden_compare_trees)
  cmake_parse_arguments(
    PARSE_ARGV 0 arg "" "EXPECTED_DIR;ACTUAL_DIR;SCRATCH_DIR;VERSION;VERSIONED;SIZE_ONLY" "EXCLUDE"
  )
  file(GLOB_RECURSE expected_files RELATIVE "${arg_EXPECTED_DIR}" "${arg_EXPECTED_DIR}/*")
  file(GLOB_RECURSE actual_files RELATIVE "${arg_ACTUAL_DIR}" "${arg_ACTUAL_DIR}/*")
  if(arg_EXCLUDE)
    list(REMOVE_ITEM actual_files ${arg_EXCLUDE})
  endif()
  list(SORT expected_files)
  list(SORT actual_files)

  set(missing_files)
  foreach(file IN LISTS expected_files)
    if(NOT EXISTS "${arg_ACTUAL_DIR}/${file}")
      list(APPEND missing_files "${file}")
    endif()
  endforeach()
  if(missing_files)
    list(JOIN missing_files "', '" missing_files_display)
    message(FATAL_ERROR "expected output not produced: '${missing_files_display}'")
  endif()
  set(extra_files)
  foreach(file IN LISTS actual_files)
    if(NOT EXISTS "${arg_EXPECTED_DIR}/${file}")
      list(APPEND extra_files "${file}")
    endif()
  endforeach()
  if(extra_files)
    list(JOIN extra_files "', '" extra_files_display)
    message(FATAL_ERROR "output not expected: '${extra_files_display}'")
  endif()

  foreach(file IN LISTS expected_files)
    if(arg_SIZE_ONLY AND file MATCHES "${arg_SIZE_ONLY}")
      file(SIZE "${arg_ACTUAL_DIR}/${file}" actual_len)
      if(actual_len EQUAL 0)
        message(FATAL_ERROR "generated output is empty: '${file}'")
      endif()
      continue()
    endif()
    set(expected_path "${arg_EXPECTED_DIR}/${file}")
    if(arg_VERSIONED AND file MATCHES "${arg_VERSIONED}")
      file(READ "${expected_path}" expected_text)
      string(REPLACE "@PROJECT_VERSION@" "${arg_VERSION}" expected_text "${expected_text}")
      set(expected_path "${arg_SCRATCH_DIR}/expected-file")
      file(WRITE "${expected_path}" "${expected_text}")
    endif()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E compare_files "${expected_path}" "${arg_ACTUAL_DIR}/${file}"
      RESULT_VARIABLE compare_result
      OUTPUT_QUIET ERROR_QUIET
    )
    if(NOT compare_result EQUAL 0)
      message(FATAL_ERROR "output differs from expected: '${file}'")
    endif()
  endforeach()
endfunction()
