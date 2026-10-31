# This file configures formatting and static analysis for first-party sources. Each project calls
# `apps_add_lint_targets` once its targets are defined, and enables analysis per target with
# `apps_enable_project_analysis` and `apps_enable_test_analysis`.

include_guard(GLOBAL)

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH apps_root_dir)

find_program(APPS_CLANG_FORMAT NAMES clang-format-23 clang-format)
find_program(APPS_CWRAP NAMES cwrap)
find_program(APPS_CLANG_TIDY NAMES clang-tidy-23 clang-tidy)
find_program(APPS_CPPCHECK NAMES cppcheck)

set(apps_clang_tidy_command "${APPS_CLANG_TIDY}")
set(apps_cppcheck_command "${APPS_CPPCHECK}")
set(apps_cppcheck_standard "c17")
if(CMAKE_CROSSCOMPILING)
  message(VERBOSE "cross compiling; clang-tidy and cppcheck are disabled")
  set(apps_clang_tidy_command "")
  set(apps_cppcheck_command "")
endif()

# clang-tidy reads the compiler's command line, and it rejects GCC-only warning options such as
# `-Wlogical-op` as errors. cppcheck does not read the flags, so it runs with any compiler.
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(VERBOSE "compiler is ${CMAKE_C_COMPILER_ID}, not Clang; clang-tidy is disabled")
  set(apps_clang_tidy_command "")
endif()

# Production and test targets run cppcheck with the same arguments.
set(apps_cppcheck_property
    "${apps_cppcheck_command};--enable=warning,performance,portability;--std=${apps_cppcheck_standard};--error-exitcode=1;--quiet"
)

# CMake hands cppcheck only the `-I` include directories, not the `-isystem` ones, and an app's
# build adds the libraries as `SYSTEM`, so cppcheck gets their include roots from `APPS_LIBRARIES`,
# the libraries that the app and its tests link, which the app sets. Without the root of `core`,
# cppcheck cannot expand `STR_FMT` and miscounts `printf` arguments. A library that the app does not
# link stays out, so a header of the app with the same path as one of that library is the one
# cppcheck reads.
function(apps_cppcheck_arguments arguments_out)
  set(arguments "${apps_cppcheck_property}")
  foreach(library IN LISTS APPS_LIBRARIES)
    list(APPEND arguments "-I${CMAKE_BINARY_DIR}/libs/include/${library}")
  endforeach()
  set(${arguments_out} "${arguments}" PARENT_SCOPE)
endfunction()

function(apps_enable_project_analysis target)
  if(apps_clang_tidy_command)
    set_property(
      TARGET ${target}
      PROPERTY C_CLANG_TIDY
               "${apps_clang_tidy_command};--quiet;--config-file=${apps_root_dir}/.clang-tidy"
    )
  endif()
  if(apps_cppcheck_command)
    apps_cppcheck_arguments(arguments)
    set_property(TARGET ${target} PROPERTY C_CPPCHECK "${arguments}")
  endif()
endfunction()

function(apps_enable_test_analysis target)
  if(apps_cppcheck_command)
    apps_cppcheck_arguments(arguments)
    set_property(TARGET ${target} PROPERTY C_CPPCHECK "${arguments}")
  endif()
endfunction()

# Sets `sources_out` to the project's own C sources and headers: each `.c` or `.h` file in the
# `SOURCES`, the `INTERFACE_SOURCES` or a header set of a target that the project's directories
# define, below `PROJECT_SOURCE_DIR` and outside `PROJECT_BINARY_DIR`, so a vendored or a generated
# file stays out. The targets give the list, so a new or renamed file needs no edit here, and a
# target that the configuration leaves out, such as a test without `APPS_BUILD_TESTING`, leaves its
# files out. A `$<TARGET_OBJECTS:...>` entry names object files, so it is skipped. Any other entry
# that starts with a generator expression has no path until generation, so a target below
# `PROJECT_SOURCE_DIR` warns that lint skips it.
function(apps_collect_lint_sources sources_out)
  set(sources "")
  set(dirs "${PROJECT_SOURCE_DIR}")
  while(dirs)
    list(POP_FRONT dirs dir)
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    list(APPEND dirs ${subdirs})
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
      get_target_property(target_dir ${target} SOURCE_DIR)
      cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${target_dir}" NORMALIZE is_first_party_target)
      set(files "")
      foreach(sources_property IN ITEMS SOURCES INTERFACE_SOURCES)
        get_target_property(property_files ${target} ${sources_property})
        if(property_files)
          list(APPEND files ${property_files})
        endif()
      endforeach()
      foreach(sets_property IN ITEMS HEADER_SETS INTERFACE_HEADER_SETS)
        get_target_property(header_sets ${target} ${sets_property})
        foreach(header_set IN LISTS header_sets)
          get_target_property(headers ${target} HEADER_SET_${header_set})
          list(APPEND files ${headers})
        endforeach()
      endforeach()
      foreach(file IN LISTS files)
        if(file MATCHES "^\\$<")
          if(is_first_party_target AND NOT file MATCHES "^\\$<TARGET_OBJECTS:")
            message(WARNING "lint skips a source of target '${target}' that starts with a "
                            "generator expression: '${file}'"
            )
          endif()
          continue()
        endif()
        if(NOT file MATCHES "\\.[ch]$")
          continue()
        endif()
        cmake_path(ABSOLUTE_PATH file BASE_DIRECTORY "${target_dir}" NORMALIZE OUTPUT_VARIABLE path)
        cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${path}" NORMALIZE is_first_party)
        cmake_path(IS_PREFIX PROJECT_BINARY_DIR "${path}" NORMALIZE is_generated)
        if(is_first_party AND NOT is_generated)
          list(APPEND sources "${path}")
        endif()
      endforeach()
    endforeach()
  endwhile()
  list(REMOVE_DUPLICATES sources)
  list(SORT sources)
  set(${sources_out} "${sources}" PARENT_SCOPE)
endfunction()

# Creates the `format` and `lint` targets over the sources that `apps_collect_lint_sources` finds,
# so the project calls it after it defines its last target. `CONFIGURED_SOURCES` lists `.c.in`
# templates, which clang-format reads as the `.c` files they configure.
#
# The format and lint targets run clang-format and then cwrap, which refills comment prose to 100
# columns, the limit that clang-format only caps. A tool that is not installed is skipped.
function(apps_add_lint_targets)
  cmake_parse_arguments(PARSE_ARGV 0 arg "" "" "CONFIGURED_SOURCES")
  apps_collect_lint_sources(arg_SOURCES)

  set(clang_format_fix "")
  set(clang_format_check "")
  if(APPS_CLANG_FORMAT)
    list(APPEND clang_format_fix COMMAND "${APPS_CLANG_FORMAT}" -i ${arg_SOURCES})
    list(APPEND clang_format_check COMMAND "${APPS_CLANG_FORMAT}" --dry-run --Werror ${arg_SOURCES})
    foreach(configured IN LISTS arg_CONFIGURED_SOURCES)
      cmake_path(GET configured STEM LAST_ONLY assumed)
      list(APPEND clang_format_fix COMMAND "${APPS_CLANG_FORMAT}" -i "--assume-filename=${assumed}"
           "${configured}"
      )
      list(APPEND clang_format_check COMMAND "${APPS_CLANG_FORMAT}" --dry-run --Werror
           "--assume-filename=${assumed}" "${configured}"
      )
    endforeach()
  else()
    set(clang_format_fix COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found, skipping")
    set(clang_format_check ${clang_format_fix})
  endif()

  if(APPS_CWRAP)
    set(cwrap_fix COMMAND "${APPS_CWRAP}" -i ${arg_SOURCES} ${arg_CONFIGURED_SOURCES})
    set(cwrap_check COMMAND "${APPS_CWRAP}" --check ${arg_SOURCES} ${arg_CONFIGURED_SOURCES})
  else()
    set(cwrap_fix COMMAND "${CMAKE_COMMAND}" -E echo "cwrap not found, skipping")
    set(cwrap_check ${cwrap_fix})
  endif()

  add_custom_target(
    format ${clang_format_fix} ${cwrap_fix}
    COMMENT "Formatting first-party sources"
    COMMAND_EXPAND_LISTS VERBATIM
  )
  add_custom_target(
    lint ${clang_format_check} ${cwrap_check}
    COMMENT "Checking first-party source formatting"
    COMMAND_EXPAND_LISTS VERBATIM
  )
endfunction()
