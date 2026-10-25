target_sources(
  fram_app
  PRIVATE album_scan.c derivative_renderer.c manifest_builder.c page_renderer.c site_writer.c
          template_context.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES album_scan.h derivative_renderer.h manifest_builder.h page_renderer.h
               site_writer.h template_context.h
)

if(APPS_BUILD_TESTING)
  # The video cases of the scan and the renderer are executables of their own on the POSIX host:
  # a video's frame is a file on the real host, and the scan's cases run `ffprobe` and `ffmpeg`
  # fakes.
  foreach(module IN ITEMS album_scan_video derivative_renderer_video)
    apps_add_unit_test(${module})
  endforeach()
  # These read and write only through `os/os.h`, so they run against the in-memory host.
  foreach(module IN ITEMS album_scan derivative_renderer manifest_builder page_renderer
                          site_writer template_context
  )
    apps_add_unit_test(${module} OS memory)
  endforeach()
endif()
