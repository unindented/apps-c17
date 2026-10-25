target_sources(
  sosig_app
  PRIVATE entry_renderer.c manifest_builder.c page_renderer.c site_writer.c template_context.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES entry_renderer.h manifest_builder.h page_renderer.h site_writer.h template_context.h
)

if(APPS_BUILD_TESTING)
  # These read and write only through `os/os.h`, so they run against the in-memory host.
  foreach(module IN ITEMS entry_renderer manifest_builder page_renderer site_writer template_context)
    apps_add_unit_test(${module} OS memory)
  endforeach()
  # These render the templates of a golden site, which only the real filesystem holds.
  apps_add_unit_test(template_context_posix)
endif()
