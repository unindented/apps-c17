target_sources(
  cwrap_app
  PRIVATE cli.c cli_dispatch.c cmd_wrap.c memory.c wrap_input.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES cli.h cli_dispatch.h cmd_wrap.h cwrap_version.h memory.h wrap_input.h
)

if(APPS_BUILD_TESTING)
  foreach(module IN ITEMS cli cli_dispatch)
    apps_add_unit_test(${module})
  endforeach()
  # A run reads, writes and prints only through `os/os.h`, so its tests use the in-memory host, as the
  # out-of-memory report's do.
  foreach(module IN ITEMS cmd_wrap memory wrap_input)
    apps_add_unit_test(${module} OS memory)
  endforeach()
endif()
