# This script makes the fixture repositories in a scratch tree of its own and checks that each one
# holds exactly the refs, with the object ids, that `tests/repos/<name>.refs` lists, so a change to
# a stream or to `surge_make_fixture_repos.cmake` shows up as a change of the ids that the golden
# tests rely on. It also runs a strict `git fsck` on the repositories that hold no hostile object.

foreach(required IN ITEMS SURGE_GIT SURGE_FIXTURE_STREAMS_DIR SURGE_SCRATCH_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

set(repos_dir "${SURGE_SCRATCH_DIR}/repos")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" "-DSURGE_GIT=${SURGE_GIT}"
    "-DSURGE_FIXTURE_STREAMS_DIR=${SURGE_FIXTURE_STREAMS_DIR}"
    "-DSURGE_FIXTURE_REPOS_DIR=${repos_dir}" -P
    "${CMAKE_CURRENT_LIST_DIR}/surge_make_fixture_repos.cmake"
  RESULT_VARIABLE make_result
  ERROR_VARIABLE make_error
)
if(NOT make_result EQUAL 0)
  message(FATAL_ERROR "making the fixture repositories failed:\n${make_error}")
endif()

# git reads these repositories as the make script wrote them, with no config of the machine and no
# repository of the environment.
set(ENV{GIT_CONFIG_NOSYSTEM} 1)
set(ENV{HOME} "${SURGE_SCRATCH_DIR}")
set(ENV{XDG_CONFIG_HOME} "${SURGE_SCRATCH_DIR}/.config")
foreach(variable IN ITEMS GIT_DIR GIT_WORK_TREE GIT_CONFIG_GLOBAL GIT_CONFIG_PARAMETERS)
  unset(ENV{${variable}})
endforeach()

file(GLOB expected_refs_files "${SURGE_FIXTURE_STREAMS_DIR}/*.refs")
foreach(expected_refs_file IN LISTS expected_refs_files)
  cmake_path(GET expected_refs_file STEM LAST_ONLY name)
  execute_process(
    COMMAND "${SURGE_GIT}" for-each-ref "--format=%(objectname) %(objecttype) %(refname)"
    WORKING_DIRECTORY "${repos_dir}/${name}"
    RESULT_VARIABLE refs_result
    OUTPUT_VARIABLE actual_refs
    ERROR_VARIABLE refs_error
  )
  if(NOT refs_result EQUAL 0)
    message(FATAL_ERROR "git for-each-ref failed in fixture '${name}':\n${refs_error}")
  endif()
  file(READ "${expected_refs_file}" expected_refs)
  if(NOT actual_refs STREQUAL expected_refs)
    message(
      FATAL_ERROR
        "fixture '${name}' holds other refs than '${expected_refs_file}':\n${actual_refs}"
    )
  endif()
endforeach()

foreach(name IN ITEMS showcase monorepo links years)
  execute_process(
    COMMAND "${SURGE_GIT}" fsck --strict --no-dangling
    WORKING_DIRECTORY "${repos_dir}/${name}"
    RESULT_VARIABLE fsck_result
    OUTPUT_VARIABLE fsck_output
    ERROR_VARIABLE fsck_output
  )
  if(NOT fsck_result EQUAL 0)
    message(FATAL_ERROR "git fsck --strict failed in fixture '${name}':\n${fsck_output}")
  endif()
endforeach()
