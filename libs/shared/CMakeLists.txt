# `shared`: the arena allocator, counted strings, growable buffers and the error log.

add_library(shared STATIC)
target_sources(
  shared
  PRIVATE src/arena.c src/buf.c src/log.c src/str.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
         FILES include/shared/arena.h include/shared/buf.h include/shared/log.h
               include/shared/str.h
)
target_compile_features(shared PUBLIC c_std_17)
# The library compiles with the build profile of the project that builds it, so an app's sanitizer
# build also instruments the arena, and the arena's ASan poisoning runs in that build.
target_link_libraries(shared PRIVATE apps_build_project)
# A static library instrumented with a sanitizer needs the sanitizer runtime in the final link, so
# whatever links the library gets the same link flags.
target_link_options(shared PUBLIC ${apps_sanitizer_flags})

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/include/shared/arena.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/include/shared/buf.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/include/shared/log.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/include/shared/str.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/arena.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/asan.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/buf.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/log.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/str.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/test_arena.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/test_buf.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/test_format_compile.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/test_log.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/test_oom.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/src/test_str.c"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(shared)

if(NOT libs_build_testing)
  return()
endif()

foreach(module IN ITEMS arena buf log str)
  libs_add_unit_test(shared ${module} SOURCE src/test_${module}.c)
endforeach()

# The `format` attribute on `buf_printf` and `log_error` must make the compiler refuse a call
# whose arguments disagree with its format. Each case compiles `src/test_format_compile.c` with
# one macro defined and expects the compiler error in `expect`, in Clang's or GCC's wording. The
# control case defines none and must compile. Fields are separated by `~`, because a regex holds
# `|`.
set(shared_format_compile_cases
    "control~"
    "raw_str~TEST_FORMAT_RAW_STR~type 'struct Str'"
    "missing_argument~TEST_FORMAT_MISSING_ARGUMENT~more '%' conversions than data arguments|expects a matching"
    "ptrdiff_as_int~TEST_FORMAT_PTRDIFF_AS_INT~type 'ptrdiff_t'"
    "nonliteral~TEST_FORMAT_NONLITERAL~format( string is)? not a string literal"
    "fmt_of_int~TEST_FORMAT_FMT_OF_INT~incompatible type"
    "wrapper~TEST_FORMAT_WRAPPER~type 'struct Str'"
)
foreach(compile_case IN LISTS shared_format_compile_cases)
  string(REPLACE "~" ";" compile_case "${compile_case}")
  list(GET compile_case 0 name)
  list(GET compile_case 1 define)
  set(expect "")
  if(define)
    list(GET compile_case 2 expect)
  endif()
  add_test(
    NAME "shared.format_compile.${name}"
    COMMAND
      "${CMAKE_COMMAND}" "-DCOMPILER=${CMAKE_C_COMPILER}"
      "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/test_format_compile.c"
      "-DINCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include" "-DCASE=${define}" "-DEXPECT=${expect}"
      "-DSYSROOT=${CMAKE_OSX_SYSROOT}" -P
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shared_compile_fail.cmake"
  )
  set_tests_properties("shared.format_compile.${name}" PROPERTIES LABELS "shared;unit")
endforeach()
