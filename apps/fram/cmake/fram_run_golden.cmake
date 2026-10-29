# This script builds one copied fixture gallery three times: on one worker, on two with the output
# cache, and on eight with `--force`, which ignores it. Each later build must leave the output as
# the first left it, so every output comes out the same whatever the worker count. A dry run after
# them must keep every output but the output cache. The script then compares the result with the
# expected output, and requires the first build's `stderr` to be the warnings in
# `<expected>.warnings.txt`, or empty without that file. The steps that the golden tests share are
# in `cmake/apps_golden.cmake`. The release workflow runs it against a packaged binary, with
# `FRAM_EMULATOR` naming a launcher such as `qemu-aarch64` when the binary targets another
# architecture.

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/apps_golden.cmake")

foreach(required IN ITEMS FRAM_EXECUTABLE FRAM_VERSION FRAM_GALLERY_DIR FRAM_EXPECTED_DIR
                        FRAM_SCRATCH_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${FRAM_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${FRAM_SCRATCH_DIR}")
file(COPY "${FRAM_GALLERY_DIR}/" DESTINATION "${FRAM_SCRATCH_DIR}")

set(actual_dir "${FRAM_SCRATCH_DIR}/public")
set(first_dir "${FRAM_SCRATCH_DIR}/first/public")

# A stale page and a stale directory tree sit in the output before the first build, as an earlier
# build of other sources would leave them. The build removes them, so the comparison below fails on
# either as an output not expected.
file(WRITE "${actual_dir}/stale.html" "stale")
file(WRITE "${actual_dir}/stale-dir/nested/stale.txt" "stale")

apps_golden_run_build(
  EXECUTABLE "${FRAM_EXECUTABLE}"
  EMULATOR ${FRAM_EMULATOR}
  WORKING_DIRECTORY "${FRAM_SCRATCH_DIR}"
  LABEL "first fixture gallery"
  ARGS --workers 1
  STDERR first_stderr
)
set(expected_warnings "")
if(EXISTS "${FRAM_EXPECTED_DIR}.warnings.txt")
  file(READ "${FRAM_EXPECTED_DIR}.warnings.txt" expected_warnings)
endif()
if(NOT first_stderr STREQUAL expected_warnings)
  message(FATAL_ERROR "the first build's warnings differ from expected:\n${first_stderr}")
endif()
file(COPY "${actual_dir}" DESTINATION "${FRAM_SCRATCH_DIR}/first")

foreach(build IN ITEMS second third)
  set(build_args --workers 2)
  if(build STREQUAL "third")
    set(build_args --workers 8 --force)
  endif()
  apps_golden_run_build(
    EXECUTABLE "${FRAM_EXECUTABLE}"
    EMULATOR ${FRAM_EMULATOR}
    WORKING_DIRECTORY "${FRAM_SCRATCH_DIR}"
    LABEL "${build} fixture gallery"
    ARGS ${build_args}
  )
  apps_golden_check_unchanged(
    FIRST_DIR "${first_dir}" ACTUAL_DIR "${actual_dir}" LABEL "${build}"
  )
endforeach()

apps_golden_run_build(
  EXECUTABLE "${FRAM_EXECUTABLE}"
  EMULATOR ${FRAM_EMULATOR}
  WORKING_DIRECTORY "${FRAM_SCRATCH_DIR}"
  LABEL "dry run of the fixture gallery"
  ARGS --dry-run
  STDOUT dry_run_listing
)
apps_golden_check_dry_run_keeps(
  LISTING "${dry_run_listing}" CACHE_PATH "public/_fram/output-cache.tsv"
)

# Generated WebP bytes vary across libwebp versions and SIMD paths, so a derivative need only be
# non-empty: the list check pins its path, and unit tests pin dimensions and pixels. An original is
# a byte copy of its source, so it is compared exactly. Pages embed the project version, so the
# expected pages hold a placeholder that a release does not have to update.
apps_golden_compare_trees(
  EXPECTED_DIR "${FRAM_EXPECTED_DIR}"
  ACTUAL_DIR "${actual_dir}"
  SCRATCH_DIR "${FRAM_SCRATCH_DIR}"
  VERSION "${FRAM_VERSION}"
  VERSIONED "\\.html$"
  SIZE_ONLY "^_fram/v[^/]*/.*\\.webp$"
  EXCLUDE "_fram/output-cache.tsv"
)
