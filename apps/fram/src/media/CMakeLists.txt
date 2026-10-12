target_sources(
  fram_app
  PRIVATE exif.c image.c proc.c video.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES exif.h image.h proc.h video.h
)

target_link_libraries(fram_app PRIVATE vendor_libjpeg_turbo vendor_libwebp vendor_stb)

if(APPS_BUILD_TESTING)
  # Processes and video frames always run on the real host.
  foreach(module IN ITEMS exif image proc video)
    apps_add_unit_test(${module})
  endforeach()
endif()
