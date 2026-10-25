target_sources(
  fram_app
  PRIVATE album.c build_cache.c gallery_config.c media_item.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES album.h build_cache.h fram_version.h gallery_config.h media_item.h
)

if(APPS_BUILD_TESTING)
  foreach(module IN ITEMS album build_cache media_item)
    apps_add_unit_test(${module})
  endforeach()
  # The config is read and printed only through `os/os.h`, so its tests run against the in-memory
  # host.
  apps_add_unit_test(gallery_config OS memory)
endif()
