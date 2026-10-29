target_sources(
  surge_app
  PRIVATE cli_spec.c cmd_build.c cmd_config.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES cli_spec.h cmd_build.h cmd_config.h
)

if(APPS_BUILD_TESTING)
  foreach(module IN ITEMS cli_spec cmd_config)
    apps_add_unit_test(${module} OS memory)
  endforeach()
  # A build reads its repositories through libgit2, outside `os/os.h`, so it runs on the POSIX host,
  # after `surge.fixture_repos_setup` in `src/vcs/` makes the fixture repositories, which needs git.
  find_program(SURGE_GIT git)
  if(SURGE_GIT)
    apps_add_unit_test(cmd_build)
    set_tests_properties(
      surge.cmd_build
      PROPERTIES FIXTURES_REQUIRED surge_fixture_repos
                 ENVIRONMENT "SURGE_FIXTURE_REPOS_DIR=${PROJECT_BINARY_DIR}/fixture-repos/$<CONFIG>"
    )
  endif()
endif()
