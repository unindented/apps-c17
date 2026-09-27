target_sources(
  fram_app
  PRIVATE proc.c video.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES proc.h video.h
)

if(APPS_BUILD_TESTING)
  # Processes and video frames always run on the real host.
  foreach(module IN ITEMS proc video)
    apps_add_unit_test(${module})
  endforeach()
endif()
