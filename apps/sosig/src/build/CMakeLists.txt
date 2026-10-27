target_sources(
  sosig_app
  PRIVATE entry_renderer.c output_manifest.c page_renderer.c site_writer.c template_context.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES entry_renderer.h output_manifest.h page_renderer.h site_writer.h template_context.h
)

if(APPS_BUILD_TESTING)
  # These read and write only through `os/os.h`, so they run against the in-memory host.
  foreach(module IN ITEMS entry_renderer output_manifest page_renderer site_writer template_context)
    apps_add_unit_test(${module} OS memory)
  endforeach()
  # These render the templates of a golden site, which only the real filesystem holds.
  apps_add_unit_test(template_context_posix)
endif()
