target_sources(
  fram_app
  PRIVATE cli.c cli_dispatch.c cmd_build.c cmd_config.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES cli.h cli_dispatch.h cmd_build.h cmd_config.h
)

if(APPS_BUILD_TESTING)
  # The builds of galleries with videos run `ffprobe` and `ffmpeg` fakes, and a video's frame is a
  # file on the real host, so these run on the POSIX one.
  apps_add_unit_test(cmd_build_video)
  # These read, walk, write and print only through `os/os.h`, so they run against the in-memory
  # host.
  foreach(module IN ITEMS cli cli_dispatch cmd_build cmd_config)
    apps_add_unit_test(${module} OS memory)
  endforeach()
endif()
