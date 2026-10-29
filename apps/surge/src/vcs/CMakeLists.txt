# The only directory whose code includes `<git2.h>`. Its reads of git objects go through libgit2,
# outside `os/os.h`, so its tests run on the POSIX host.
target_sources(
  surge_app
  PRIVATE changeset.c filetree.c pack_export.c refset.c repository.c revision.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES changeset.h filetree.h pack_export.h refset.h repository.h revision.h
)
target_link_libraries(surge_app PRIVATE vendor_libgit2)

if(APPS_BUILD_TESTING)
  # The tests that read a repository share the fixture repositories, which one setup test makes
  # from their streams before them, so they need git.
  find_program(SURGE_GIT git)
  if(SURGE_GIT)
    set(fixture_repos_dir "${PROJECT_BINARY_DIR}/fixture-repos/$<CONFIG>")
    add_test(
      NAME surge.fixture_repos_setup
      COMMAND
        "${CMAKE_COMMAND}" "-DSURGE_GIT=${SURGE_GIT}"
        "-DSURGE_FIXTURE_STREAMS_DIR=${PROJECT_SOURCE_DIR}/tests/repos"
        "-DSURGE_FIXTURE_REPOS_DIR=${fixture_repos_dir}" -P
        "${PROJECT_SOURCE_DIR}/cmake/surge_make_fixture_repos.cmake"
    )
    set_tests_properties(
      surge.fixture_repos_setup PROPERTIES LABELS "surge;unit" FIXTURES_SETUP surge_fixture_repos
    )
    foreach(module IN ITEMS changeset filetree pack_export refset repository revision)
      apps_add_unit_test(${module})
      target_link_libraries(surge_unit_${module} PRIVATE vendor_libgit2)
      set_tests_properties(
        "surge.${module}"
        PROPERTIES FIXTURES_REQUIRED surge_fixture_repos
                   ENVIRONMENT "SURGE_FIXTURE_REPOS_DIR=${fixture_repos_dir}"
      )
    endforeach()
  endif()
endif()
