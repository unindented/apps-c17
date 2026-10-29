target_sources(
  surge_app
  PRIVATE build_cache.c grammar_path.c semver.c site_config.c timestamp.c url_segment.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES build_cache.h
               grammar_path.h
               semver.h
               repo_model.h
               site_config.h
               surge_version.h
               timestamp.h
               url_segment.h
)

if(APPS_BUILD_TESTING)
  foreach(module IN ITEMS build_cache grammar_path semver timestamp url_segment)
    apps_add_unit_test(${module})
  endforeach()
  # The config reads its file and prints only through `os/os.h`, so its tests use the in-memory
  # host.
  apps_add_unit_test(site_config OS memory)
endif()
