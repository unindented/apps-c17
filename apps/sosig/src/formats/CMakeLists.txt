target_sources(
  sosig_app
  PRIVATE body_html.c highlight.c html.c markdown.c org.c toml.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES body_html.h highlight.h html.h markdown.h org.h toml.h
)

target_link_libraries(
  sosig_app
  PRIVATE vendor_md4c
          vendor_org4c
          vendor_tree_sitter_grammars
          vendor_tree_sitter
)

if(APPS_BUILD_TESTING)
  foreach(module IN ITEMS body_html html markdown org toml)
    apps_add_unit_test(${module})
  endforeach()
  apps_add_unit_test(highlight OS memory)
endif()
