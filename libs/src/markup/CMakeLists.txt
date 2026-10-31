# `markup`: Markdown and Org to HTML, with the markup both renderers share, and the syntax
# highlighting of code blocks with tree-sitter.

add_library(markup STATIC)
target_sources(
  markup
  PRIVATE body_html.c
          highlight.c
          markdown.c
          org.c
          body_html.h
          highlight.h
          markdown.h
          org.h
)
libs_add_include_root(markup markup)
target_link_libraries(
  markup
  PUBLIC core
  PRIVATE apps_build_project
          formats
          vendor_md4c
          vendor_org4c
          vendor_tree_sitter_grammars
          vendor_tree_sitter
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(markup)

if(NOT libs_build_testing)
  return()
endif()

foreach(module IN ITEMS body_html markdown org)
  libs_add_unit_test(markup ${module} LIBRARIES testing)
endforeach()
libs_add_unit_test(markup highlight OS memory LIBRARIES testing)
