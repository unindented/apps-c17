target_sources(
  sosig_app
  PRIVATE cli_spec.c cmd_build.c cmd_config.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES cli_spec.h cmd_build.h cmd_config.h
)

if(APPS_BUILD_TESTING)
  # The commands read, walk, write and print only through `os/os.h`, so their tests use the
  # in-memory host.
  foreach(module IN ITEMS cli_spec cmd_build cmd_config)
    apps_add_unit_test(${module} OS memory)
  endforeach()
endif()
