target_sources(
  fram_app
  PRIVATE album_scanner.c derivative_renderer.c manifest_builder.c page_renderer.c site_writer.c
          template.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES album_scanner.h derivative_renderer.h manifest_builder.h page_renderer.h
               site_writer.h template.h
)
target_link_libraries(fram_app PRIVATE vendor_mustache)

if(APPS_BUILD_TESTING)
  foreach(module IN ITEMS template)
    apps_add_unit_test(${module})
  endforeach()
  # The video cases of the scanner and the renderer are executables of their own on the POSIX host:
  # a video's frame is a file on the real host, and the scanner's cases run `ffprobe` and `ffmpeg`
  # fakes.
  foreach(module IN ITEMS album_scanner_video derivative_renderer_video)
    apps_add_unit_test(${module})
  endforeach()
  # These read and write only through `runtime/os.h`, so they run against the in-memory host.
  foreach(module IN ITEMS album_scanner derivative_renderer manifest_builder page_renderer
                          site_writer
  )
    apps_add_unit_test(${module} OS memory)
  endforeach()
endif()
