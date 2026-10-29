# This script builds one copied fixture site three times: on one worker, on two, which keeps what
# did not change, and on eight with `--force`, which keeps nothing. Each later build must leave the
# output as the first left it, so every output comes out the same whatever the worker count. A dry
# run after them must keep every output. The script then compares the result with the expected
# output byte for byte, and requires the first build's `stderr` to be the warnings in
# `<expected>.warnings.txt`, or empty without that file. The steps that the golden tests share are
# in `cmake/apps_golden.cmake`. The release workflow runs it against a packaged binary, with
# `SOSIG_EMULATOR` naming a launcher such as `qemu-aarch64` when the binary targets another
# architecture.

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/apps_golden.cmake")

foreach(required IN ITEMS SOSIG_EXECUTABLE SOSIG_SITE_DIR SOSIG_EXPECTED_DIR SOSIG_SCRATCH_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${SOSIG_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SOSIG_SCRATCH_DIR}")
file(COPY "${SOSIG_SITE_DIR}/" DESTINATION "${SOSIG_SCRATCH_DIR}")

set(actual_dir "${SOSIG_SCRATCH_DIR}/public")
set(first_dir "${SOSIG_SCRATCH_DIR}/first/public")

# A stale page and a stale directory tree sit in the output before the first build, as an earlier
# build of other sources would leave them. The build removes them, so the comparison below fails on
# either as an output not expected.
file(WRITE "${actual_dir}/stale.html" "stale")
file(WRITE "${actual_dir}/stale-dir/nested/stale.txt" "stale")

apps_golden_run_build(
  EXECUTABLE "${SOSIG_EXECUTABLE}"
  EMULATOR ${SOSIG_EMULATOR}
  WORKING_DIRECTORY "${SOSIG_SCRATCH_DIR}"
  LABEL "first fixture site"
  ARGS --workers 1
  STDERR first_stderr
)
set(expected_warnings "")
if(EXISTS "${SOSIG_EXPECTED_DIR}.warnings.txt")
  file(READ "${SOSIG_EXPECTED_DIR}.warnings.txt" expected_warnings)
endif()
if(NOT first_stderr STREQUAL expected_warnings)
  message(FATAL_ERROR "the first build's warnings differ from expected:\n${first_stderr}")
endif()
file(COPY "${actual_dir}" DESTINATION "${SOSIG_SCRATCH_DIR}/first")

foreach(build IN ITEMS second third)
  set(build_args --workers 2)
  if(build STREQUAL "third")
    set(build_args --workers 8 --force)
  endif()
  apps_golden_run_build(
    EXECUTABLE "${SOSIG_EXECUTABLE}"
    EMULATOR ${SOSIG_EMULATOR}
    WORKING_DIRECTORY "${SOSIG_SCRATCH_DIR}"
    LABEL "${build} fixture site"
    ARGS ${build_args}
  )
  apps_golden_check_unchanged(
    FIRST_DIR "${first_dir}" ACTUAL_DIR "${actual_dir}" LABEL "${build}"
  )
endforeach()

apps_golden_run_build(
  EXECUTABLE "${SOSIG_EXECUTABLE}"
  EMULATOR ${SOSIG_EMULATOR}
  WORKING_DIRECTORY "${SOSIG_SCRATCH_DIR}"
  LABEL "dry run of the fixture site"
  ARGS --dry-run
  STDOUT dry_run_listing
)
apps_golden_check_dry_run_keeps(
  LISTING "${dry_run_listing}" CACHE_PATH "public/_sosig/output-cache.tsv"
)

apps_golden_compare_trees(
  EXPECTED_DIR "${SOSIG_EXPECTED_DIR}"
  ACTUAL_DIR "${actual_dir}"
  SCRATCH_DIR "${SOSIG_SCRATCH_DIR}"
  EXCLUDE "_sosig/output-cache.tsv"
)
