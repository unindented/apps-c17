target_sources(
  surge_app
  PRIVATE contributions.c
          export_writer.c
          highlight_diff.c
          page_blob.c
          page_commit.c
          page_context.c
          page_feed.c
          page_index.c
          page_log.c
          page_paths.c
          page_refs.c
          page_release.c
          page_tree.c
          readme.c
          release_scan.c
          release_writer.c
          repo_pages.c
          repo_scan.c
          template_value.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES contributions.h
               export_writer.h
               highlight_diff.h
               page_blob.h
               page_commit.h
               page_context.h
               page_feed.h
               page_index.h
               page_log.h
               page_paths.h
               page_refs.h
               page_release.h
               page_tree.h
               readme.h
               release_scan.h
               release_writer.h
               repo_pages.h
               repo_scan.h
               template_value.h
)

if(APPS_BUILD_TESTING)
  # These reach no host, so they link the POSIX one, as every app's host-free tests do.
  foreach(
    module IN
    ITEMS contributions
          highlight_diff
          page_blob
          page_commit
          page_feed
          page_log
          page_paths
          page_refs
          page_release
          page_tree
          readme
  )
    apps_add_unit_test(${module})
  endforeach()
  # Its partials are read through `os/os.h`, so it runs against the in-memory host.
  apps_add_unit_test(template_value OS memory)
  # It writes through the output writer, so it runs against the in-memory host.
  apps_add_unit_test(export_writer OS memory)
endif()
