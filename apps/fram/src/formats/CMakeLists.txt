target_sources(
  fram_app
  PRIVATE exif.c html.c image.c toml.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES exif.h html.h image.h toml.h
)

target_link_libraries(fram_app PRIVATE vendor_libjpeg_turbo vendor_libwebp vendor_stb)

if(APPS_BUILD_TESTING)
  foreach(module IN ITEMS exif html image toml)
    apps_add_unit_test(${module})
  endforeach()
endif()
