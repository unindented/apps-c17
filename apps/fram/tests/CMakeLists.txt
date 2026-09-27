# The golden test suite uses several modules. Keep it in the top-level `tests/` directory.

# fram's own test helpers, which every unit test links next to the `testing` library: the fixture
# JPEG encoder and the WebP output decoder, which call their codecs themselves.
add_library(fram_test_support OBJECT test_jpeg_fixture.c test_webp_output.c)
target_include_directories(fram_test_support PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>")
target_link_libraries(
  fram_test_support PRIVATE apps_build_tests vendor_acutest fram_app vendor_libjpeg_turbo
                            vendor_libwebp
)
if(PROJECT_IS_TOP_LEVEL)
  apps_enable_test_analysis(fram_test_support)
endif()

# Seeded random tests over the EXIF and JPEG-header parsers.
apps_add_unit_test(random)

find_program(FRAM_FFMPEG ffmpeg)
find_program(FRAM_FFPROBE ffprobe)

function(fram_add_golden_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "GALLERY;EXPECTED" "")
  add_test(
    NAME "${name}"
    COMMAND
      "${CMAKE_COMMAND}" "-DFRAM_EXECUTABLE=$<TARGET_FILE:fram>" "-DFRAM_VERSION=${PROJECT_VERSION}"
      "-DFRAM_GALLERY_DIR=${arg_GALLERY}"
      "-DFRAM_EXPECTED_DIR=${arg_EXPECTED}"
      "-DFRAM_SCRATCH_DIR=${PROJECT_BINARY_DIR}/golden/$<CONFIG>/${name}" -P
      "${PROJECT_SOURCE_DIR}/cmake/fram_run_golden.cmake"
  )
  set_tests_properties(
    "${name}"
    PROPERTIES LABELS "fram;golden"
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )
endfunction()

# The fixture gallery mixes images and MP4 sources, so its golden test needs FFmpeg.
if(FRAM_FFMPEG AND FRAM_FFPROBE)
  set(gallery_dir "${CMAKE_CURRENT_LIST_DIR}/fixtures/gallery")
  set(expected_dir "${CMAKE_CURRENT_LIST_DIR}/expected/gallery")
  fram_add_golden_test("fram.golden" GALLERY "${gallery_dir}" EXPECTED "${expected_dir}")
else()
  message(STATUS "ffmpeg or ffprobe not found; skipping golden tests")
endif()
