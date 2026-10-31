# `formats`: HTML escaping, front matter fences, TOML parsing with key checks, text checks and dates,
# and the escaping of a tab-separated field.

add_library(formats STATIC)
target_sources(
  formats PRIVATE frontmatter.c html.c toml.c toml_config.c tsv.c frontmatter.h html.h rfc3339.h toml.h tsv.h
)
libs_add_include_root(formats formats)
target_link_libraries(formats PUBLIC core os vendor_tomlc17 PRIVATE apps_build_project)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(formats)

if(NOT libs_build_testing)
  return()
endif()

foreach(module IN ITEMS frontmatter html tsv)
  libs_add_unit_test(formats ${module} LIBRARIES testing)
endforeach()
# The config read goes through `os/os.h`, so the TOML tests run against the in-memory host.
libs_add_unit_test(formats toml OS memory LIBRARIES testing)
