target_sources(
  sosig_app
  PRIVATE build_cache.c content_entry.c frontmatter.c permalink.c site_config.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES build_cache.h content_entry.h frontmatter.h permalink.h site_config.h
               sosig_version.h
)

if(APPS_BUILD_TESTING)
  foreach(module IN ITEMS build_cache content_entry frontmatter permalink site_config)
    apps_add_unit_test(${module})
  endforeach()
endif()
