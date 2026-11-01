# The golden and fixture tests use several modules. Keep them in the top-level `tests/` directory.

# The fixture repositories are made from the `git fast-import` streams in `repos/`, so every test
# that reads one needs git. Without it, the configure step leaves those tests out, as fram leaves
# out its golden test without FFmpeg.
find_program(SURGE_GIT git)
if(NOT SURGE_GIT)
  message(STATUS "git not found; the surge tests that need fixture repositories are skipped")
  return()
endif()

add_test(
  NAME surge.fixture_repos
  COMMAND
    "${CMAKE_COMMAND}" "-DSURGE_GIT=${SURGE_GIT}"
    "-DSURGE_FIXTURE_STREAMS_DIR=${CMAKE_CURRENT_LIST_DIR}/repos"
    "-DSURGE_SCRATCH_DIR=${PROJECT_BINARY_DIR}/golden/$<CONFIG>/fixture_repos" -P
    "${PROJECT_SOURCE_DIR}/cmake/surge_check_fixture_repos.cmake"
)
set_tests_properties(surge.fixture_repos PROPERTIES LABELS "surge;golden")

# The clone test serves the output with Python's HTTP server. Without Python, the golden test still
# runs, without it.
find_program(SURGE_PYTHON python3)
if(NOT SURGE_PYTHON)
  message(STATUS "python3 not found; the surge golden test skips its clone test")
endif()

# The mise test installs a release project through its example. Without mise, the golden test still
# runs, without it.
find_program(SURGE_MISE mise)
if(NOT SURGE_MISE)
  message(STATUS "mise not found; the surge golden test skips its mise test")
endif()

# Each fixture site in `fixtures/` is a golden test. A site whose expected directory holds only
# `stderr.txt` is one the build must refuse with that text.
set(surge_fixture_sites
    site
    refuse-branch-case
    refuse-project-path
    refuse-ref-prefix
    refuse-releases
    refuse-static
    refuse-templates-dir
)

foreach(site IN LISTS surge_fixture_sites)
  set(site_dir "${CMAKE_CURRENT_LIST_DIR}/fixtures/${site}")
  set(expected_dir "${CMAKE_CURRENT_LIST_DIR}/expected/${site}")
  if(NOT IS_DIRECTORY "${site_dir}")
    message(FATAL_ERROR "fixture site '${site}' is listed but '${site_dir}' does not exist")
  endif()
  set(golden_args)
  if(EXISTS "${expected_dir}/stderr.txt")
    list(APPEND golden_args "-DSURGE_EXPECTED_STDERR=${expected_dir}/stderr.txt")
  elseif(SURGE_PYTHON)
    list(APPEND golden_args "-DSURGE_PYTHON=${SURGE_PYTHON}")
    if(SURGE_MISE)
      list(APPEND golden_args "-DSURGE_MISE=${SURGE_MISE}")
    endif()
  endif()
  add_test(
    NAME "surge.golden.${site}"
    COMMAND
      "${CMAKE_COMMAND}" "-DSURGE_EXECUTABLE=$<TARGET_FILE:surge>" "-DSURGE_GIT=${SURGE_GIT}"
      "-DSURGE_SITE_DIR=${site_dir}" "-DSURGE_EXPECTED_DIR=${expected_dir}"
      "-DSURGE_FIXTURE_STREAMS_DIR=${CMAKE_CURRENT_LIST_DIR}/repos"
      "-DSURGE_VERSION=${PROJECT_VERSION}"
      "-DSURGE_SCRATCH_DIR=${PROJECT_BINARY_DIR}/golden/$<CONFIG>/${site}" ${golden_args} -P
      "${PROJECT_SOURCE_DIR}/cmake/surge_run_golden.cmake"
  )
  set_tests_properties(
    "surge.golden.${site}"
    PROPERTIES LABELS "surge;golden"
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )
endforeach()
