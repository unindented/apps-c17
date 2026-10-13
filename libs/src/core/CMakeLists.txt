# `core`: the arena allocator, counted strings, growable buffers and the error log, and ASCII
# classification, error text, number parsing, paths, path lists, text helpers and the exit codes of
# the apps.

add_library(core STATIC)
target_sources(
  core
  PRIVATE arena.c
          buf.c
          error.c
          log.c
          parse.c
          path.c
          path_list.c
          str.c
          text.c
          arena.h
          asan.h
          ascii.h
          buf.h
          error.h
          exit_code.h
          log.h
          parse.h
          path.h
          path_list.h
          str.h
          text.h
)
libs_add_include_root(core core)
target_link_libraries(core PRIVATE apps_build_project)

set_property(
  GLOBAL APPEND
  PROPERTY apps_libs_lint_sources
           "${CMAKE_CURRENT_SOURCE_DIR}/arena.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/arena.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/asan.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/ascii.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/buf.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/buf.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/error.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/error.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/exit_code.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/log.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/log.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/parse.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/parse.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/path.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/path.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/path_list.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/path_list.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/str.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/str.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_arena.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_ascii.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_buf.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_error.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_format_compile.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_log.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_oom.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_parse.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_path.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_path_list.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_str.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/test_text.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/text.c"
           "${CMAKE_CURRENT_SOURCE_DIR}/text.h"
)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(core)

if(NOT libs_build_testing)
  return()
endif()

# The tests of the arena, strings, buffers and log define their own `os_oom`, in `test_oom.h`, so
# they link no `testing`.
foreach(module IN ITEMS arena buf log str)
  libs_add_unit_test(core ${module})
endforeach()
foreach(module IN ITEMS ascii error parse path path_list text)
  libs_add_unit_test(core ${module} LIBRARIES testing)
endforeach()

# The `format` attribute on `buf_printf` and `log_error` must make the compiler refuse a call
# whose arguments disagree with its format. Each case compiles `test_format_compile.c` with
# one macro defined and expects the compiler error in `expect`, in Clang's or GCC's wording. The
# control case defines none and must compile. Fields are separated by `~`, because a regex holds
# `|`.
set(core_format_compile_cases
    "control~"
    "raw_str~TEST_FORMAT_RAW_STR~type 'struct Str'"
    "missing_argument~TEST_FORMAT_MISSING_ARGUMENT~more '%' conversions than data arguments|expects a matching"
    "ptrdiff_as_int~TEST_FORMAT_PTRDIFF_AS_INT~type 'ptrdiff_t'"
    "nonliteral~TEST_FORMAT_NONLITERAL~format( string is)? not a string literal"
    "fmt_of_int~TEST_FORMAT_FMT_OF_INT~incompatible type"
    "wrapper~TEST_FORMAT_WRAPPER~type 'struct Str'"
)
foreach(compile_case IN LISTS core_format_compile_cases)
  string(REPLACE "~" ";" compile_case "${compile_case}")
  list(GET compile_case 0 name)
  list(GET compile_case 1 define)
  set(expect "")
  if(define)
    list(GET compile_case 2 expect)
  endif()
  add_test(
    NAME "core.format_compile.${name}"
    COMMAND
      "${CMAKE_COMMAND}" "-DCOMPILER=${CMAKE_C_COMPILER}"
      "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/test_format_compile.c"
      "-DINCLUDE_DIR=${PROJECT_BINARY_DIR}/include/core" "-DCASE=${define}" "-DEXPECT=${expect}"
      "-DSYSROOT=${CMAKE_OSX_SYSROOT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail.cmake"
  )
  set_tests_properties("core.format_compile.${name}" PROPERTIES LABELS "core;unit")
endforeach()
