# This file defines the vendored targets that the `markup` library links, from the root `vendor/`
# directory: md4c, org4c, the tree-sitter runtime, and the tree-sitter grammars that `APPS_GRAMMARS`
# selects, with their highlight queries from `queries/`. Each app that links `markup` includes it
# from its own `vendor.cmake`, and the `libs/` project includes it when it is configured on its own.

include_guard(GLOBAL)

set(markup_dir "${CMAKE_CURRENT_LIST_DIR}")
cmake_path(SET markup_vendor_dir NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/../../vendor")
set(markup_vendor_binary_dir "${CMAKE_BINARY_DIR}/markup_vendor")

add_library(vendor_md4c STATIC ${markup_vendor_dir}/md4c/md4c.c)
target_include_directories(
  vendor_md4c SYSTEM PUBLIC "$<BUILD_INTERFACE:${markup_vendor_dir}/md4c>"
)

add_library(
  vendor_org4c STATIC ${markup_vendor_dir}/org4c/org4c.c ${markup_vendor_dir}/org4c/entity.c
)
target_include_directories(
  vendor_org4c SYSTEM PUBLIC "$<BUILD_INTERFACE:${markup_vendor_dir}/org4c>"
)

foreach(target IN ITEMS vendor_md4c vendor_org4c)
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE apps_build_vendor)
endforeach()

# The tree-sitter runtime is one unity source, compiled with the definitions upstream's CMake gives
# it.
add_library(vendor_tree_sitter STATIC ${markup_vendor_dir}/tree-sitter/src/lib.c)
target_include_directories(
  vendor_tree_sitter SYSTEM
  PUBLIC "$<BUILD_INTERFACE:${markup_vendor_dir}/tree-sitter/include>"
  PRIVATE "${markup_vendor_dir}/tree-sitter/src"
)
target_compile_definitions(
  vendor_tree_sitter PRIVATE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE _BSD_SOURCE
                                   _DARWIN_C_SOURCE
)
target_compile_options(
  vendor_tree_sitter PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>
)
target_link_libraries(vendor_tree_sitter PRIVATE apps_build_vendor)

# Every grammar's scanner defines its `create` function without a prototype, and the runtime calls
# it through `void *(*)(void)`, which Clang's `-fsanitize=function`, part of `-fsanitize=undefined`
# on Linux, reports as a call through the wrong type. The check is turned off where the call is
# made. It is a source option, which comes after the sanitizer options `apps_build_vendor` gives the
# target, because a later `-fsanitize` would turn the check back on.
if(APPS_SANITIZER STREQUAL "address")
  check_c_compiler_flag(-fno-sanitize=function apps_have_no_sanitize_function)
  if(apps_have_no_sanitize_function)
    set_source_files_properties(
      ${markup_vendor_dir}/tree-sitter/src/lib.c TARGET_DIRECTORY vendor_tree_sitter
      PROPERTIES COMPILE_OPTIONS -fno-sanitize=function
    )
  endif()
endif()

# The grammars the releases link, every call below in its order: systems languages, then the web's,
# a page's document, style and script, then data. The tests need them all. An empty `APPS_GRAMMARS`
# stands for them, rather than a copy of the list in the cache, so a build directory links a grammar
# that joins the default without being configured afresh.
set(markup_tree_sitter_grammars_default c bash html css javascript typescript tsx json)
set(APPS_GRAMMARS
    ""
    CACHE STRING
          "Tree-sitter grammars to link, a list of the names in libs/markup/vendor.cmake, or empty"
)
if(APPS_GRAMMARS STREQUAL "")
  set(markup_tree_sitter_grammars ${markup_tree_sitter_grammars_default})
else()
  set(markup_tree_sitter_grammars ${APPS_GRAMMARS})
endif()

# Links the selected grammars' libraries, and carries the header that declares them to the
# highlighter.
add_library(vendor_tree_sitter_grammars INTERFACE)
set(markup_tree_sitter_grammars_known "")
set(markup_tree_sitter_grammars_linked "")
set(markup_tree_sitter_grammar_declarations "")
set(markup_tree_sitter_grammar_rows "")

# Registers one tree-sitter grammar, `name`, which a code block's language finds by any of `NAMES`,
# all lowercase. A path in `SOURCES` names a file under the root `vendor/` directory, and a path in
# `QUERIES` a file in `libs/markup/queries/`. When the build selects the grammar, adds a static
# library for it from `SOURCES`, with the highlight query that `QUERIES` make, concatenated in order, and appends its declarations, its
# row of the highlighter's table and its name to the text of `tree_sitter_grammars.h`. The query is
# embedded as the byte array `tree_sitter_<name>_highlights`, `tree_sitter_<name>_highlights_len`
# bytes long, in a source generated at configure time. Reading each file as hexadecimal keeps every
# byte as it is, so neither CMake nor the C compiler expands anything in a query. The C is kept in
# strings rather than lists, because its `;` would split a list.
function(markup_add_tree_sitter_grammar name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "NAMES;SOURCES;QUERIES")
  list(LENGTH arg_NAMES name_count)
  if(name_count EQUAL 0)
    message(FATAL_ERROR "markup_add_tree_sitter_grammar(${name}) needs NAMES")
  endif()
  list(APPEND markup_tree_sitter_grammars_known ${name})
  set(markup_tree_sitter_grammars_known "${markup_tree_sitter_grammars_known}" PARENT_SCOPE)
  if(NOT name IN_LIST markup_tree_sitter_grammars)
    return()
  endif()

  list(TRANSFORM arg_SOURCES PREPEND "${markup_vendor_dir}/")
  list(TRANSFORM arg_QUERIES PREPEND "${markup_dir}/queries/")

  set(bytes "")
  foreach(query IN LISTS arg_QUERIES)
    set(path "${query}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," query_bytes "${hex}")
    string(APPEND bytes "${query_bytes}")
  endforeach()
  set(query_source "${markup_vendor_binary_dir}/tree_sitter_${name}_highlights.c")
  file(
    CONFIGURE
    OUTPUT "${query_source}"
    CONTENT
      "#include <stdint.h>\n\nconst char tree_sitter_${name}_highlights[] = {${bytes}};\nconst uint32_t tree_sitter_${name}_highlights_len = (uint32_t)sizeof(tree_sitter_${name}_highlights);\n"
    @ONLY
  )

  set(target "vendor_tree_sitter_${name}")
  add_library(${target} STATIC ${arg_SOURCES} "${query_source}")
  list(GET arg_SOURCES 0 parser)
  get_filename_component(source_dir "${parser}" DIRECTORY)
  target_include_directories(${target} PRIVATE "${source_dir}")
  # A scanner allocates through the runtime's allocator, which `highlight.c` replaces, rather than
  # through `malloc`, and classifies characters the same way on every host, through
  # `tree_sitter_wctype.h`, which explains why.
  target_compile_definitions(${target} PRIVATE TREE_SITTER_REUSE_ALLOCATOR)
  target_compile_options(
    ${target} PRIVATE -include "${markup_dir}/tree_sitter_wctype.h"
  )
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE apps_build_vendor vendor_tree_sitter)
  target_link_libraries(vendor_tree_sitter_grammars INTERFACE ${target})
  list(APPEND markup_tree_sitter_grammars_linked ${name})
  set(markup_tree_sitter_grammars_linked "${markup_tree_sitter_grammars_linked}" PARENT_SCOPE)

  set(names "${arg_NAMES}")
  list(TRANSFORM names REPLACE "(.+)" "STR_INIT(\"\\1\")")
  list(JOIN names ", " names)
  string(
    APPEND
    markup_tree_sitter_grammar_declarations
    "const TSLanguage* tree_sitter_${name}(void);\n"
    "extern const char tree_sitter_${name}_highlights[];\n"
    "extern const uint32_t tree_sitter_${name}_highlights_len;\n"
    "static const struct Str tree_sitter_${name}_names[] = {${names}};\n\n"
  )
  string(
    APPEND
    markup_tree_sitter_grammar_rows
    "  {.names = tree_sitter_${name}_names, "
    ".name_count = (ptrdiff_t)(sizeof(tree_sitter_${name}_names) / sizeof(tree_sitter_${name}_names[0])), "
    ".language = tree_sitter_${name}, "
    ".query_source = tree_sitter_${name}_highlights, "
    ".query_source_len = &tree_sitter_${name}_highlights_len}, \\\n"
  )
  set(markup_tree_sitter_grammar_declarations
      "${markup_tree_sitter_grammar_declarations}"
      PARENT_SCOPE
  )
  set(markup_tree_sitter_grammar_rows "${markup_tree_sitter_grammar_rows}" PARENT_SCOPE)
endfunction()

# Each query list is in last-wins order, the precedence tree-sitter's highlighter has used since
# 2024: on one node, the later pattern wins. Each query file holds the patterns of one upstream
# query file, as `THIRD_PARTY_NOTICES.md` records. JavaScript takes its files in the order its
# `tree-sitter.json` lists. TypeScript's `tree-sitter.json` still lists its own query before
# JavaScript's, for the first-wins precedence it was written for, so TypeScript and TSX take the
# reverse, which lets TypeScript's patterns win over JavaScript's as upstream meant. JavaScript's
# parameter patterns do not compile against either TypeScript grammar, so TypeScript and TSX end
# with `typescript_params.scm`, which captures the same parameters in their grammars.
markup_add_tree_sitter_grammar(c NAMES c SOURCES tree-sitter-c/src/parser.c QUERIES c.scm)
# The shell names are the Bourne-family names Emacs gives `sh-mode`; its csh, rc, `es` and `rpm`
# names are left out, because their syntax is not Bourne's.
markup_add_tree_sitter_grammar(
  bash
  NAMES sh shell bash bash2 ash dash jsh ksh ksh88 pdksh mksh dtksh oash wksh wsh posix zsh
  SOURCES tree-sitter-bash/src/parser.c tree-sitter-bash/src/scanner.c
  QUERIES bash.scm
)
markup_add_tree_sitter_grammar(
  html
  NAMES html
  SOURCES tree-sitter-html/src/parser.c tree-sitter-html/src/scanner.c
  QUERIES html.scm
)
markup_add_tree_sitter_grammar(
  css
  NAMES css
  SOURCES tree-sitter-css/src/parser.c tree-sitter-css/src/scanner.c
  QUERIES css.scm
)
markup_add_tree_sitter_grammar(
  javascript
  NAMES javascript js jsx
  SOURCES tree-sitter-javascript/src/parser.c tree-sitter-javascript/src/scanner.c
  QUERIES javascript.scm jsx.scm javascript_params.scm
)
markup_add_tree_sitter_grammar(
  typescript
  NAMES typescript ts
  SOURCES tree-sitter-typescript/typescript/src/parser.c
          tree-sitter-typescript/typescript/src/scanner.c
  QUERIES javascript.scm typescript.scm typescript_params.scm
)
markup_add_tree_sitter_grammar(
  tsx
  NAMES tsx
  SOURCES tree-sitter-typescript/tsx/src/parser.c tree-sitter-typescript/tsx/src/scanner.c
  QUERIES javascript.scm jsx.scm typescript.scm typescript_params.scm
)
markup_add_tree_sitter_grammar(
  json
  NAMES json
  SOURCES tree-sitter-json/src/parser.c
  QUERIES json.scm
)

# A name no call registers, or a list of no names, such as `;`, stops the configure. The count,
# rather than `if()` on the list, decides, because `if()` takes a list such as `no` as false.
set(markup_tree_sitter_grammars_unknown ${markup_tree_sitter_grammars})
list(REMOVE_ITEM markup_tree_sitter_grammars_unknown ${markup_tree_sitter_grammars_known})
list(LENGTH markup_tree_sitter_grammars markup_tree_sitter_grammars_count)
list(LENGTH markup_tree_sitter_grammars_unknown markup_tree_sitter_grammars_unknown_count)
if(markup_tree_sitter_grammars_count EQUAL 0 OR markup_tree_sitter_grammars_unknown_count GREATER 0)
  list(JOIN markup_tree_sitter_grammars_known ", " markup_tree_sitter_grammars_valid)
  list(JOIN markup_tree_sitter_grammars_unknown "', '" markup_tree_sitter_grammars_got)
  message(
    FATAL_ERROR
      "APPS_GRAMMARS must name grammars among ${markup_tree_sitter_grammars_valid}; got '${markup_tree_sitter_grammars_got}'"
  )
endif()

if(APPS_BUILD_TESTING AND NOT APPS_GRAMMARS STREQUAL "")
  message(
    FATAL_ERROR
      "the tests need every grammar, which an empty APPS_GRAMMARS selects; got '${APPS_GRAMMARS}', so set APPS_BUILD_TESTING=OFF to build it"
  )
endif()

# `highlight.c` alone includes the header, and defines its table of grammars from it.
# Its directory is a system one, as every vendored include directory is, so no analyzer checks it.
set(markup_tree_sitter_grammars_include "${markup_vendor_binary_dir}/include")
list(JOIN markup_tree_sitter_grammars_linked ", " markup_tree_sitter_grammars_linked_text)
file(
  CONFIGURE
  OUTPUT "${markup_tree_sitter_grammars_include}/tree_sitter_grammars.h"
  CONTENT
    "// Generated by `libs/markup/vendor.cmake` from `APPS_GRAMMARS`. `highlight.c` defines\n// its table of grammars from `TREE_SITTER_GRAMMARS`, and its list of them from\n// `TREE_SITTER_GRAMMAR_LIST`.\n\n#ifndef APPS_MARKUP_TREE_SITTER_GRAMMARS_H\n#define APPS_MARKUP_TREE_SITTER_GRAMMARS_H\n\n#include <stddef.h>\n#include <stdint.h>\n#include <tree_sitter/api.h>\n\n#include \"core/str.h\"\n\n${markup_tree_sitter_grammar_declarations}#define TREE_SITTER_GRAMMAR_LIST \"${markup_tree_sitter_grammars_linked_text}\"\n\n#define TREE_SITTER_GRAMMARS \\\n${markup_tree_sitter_grammar_rows}\n#endif\n"
  @ONLY
)
target_include_directories(
  vendor_tree_sitter_grammars SYSTEM
  INTERFACE "$<BUILD_INTERFACE:${markup_tree_sitter_grammars_include}>"
)
