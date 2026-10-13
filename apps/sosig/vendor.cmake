# This file builds the vendored sources that sosig uses, from the root `vendor/` directory, and
# isolates them from first-party warnings and analyzers.

cmake_path(SET vendor_dir NORMALIZE "${PROJECT_SOURCE_DIR}/../../vendor")

add_library(vendor_acutest INTERFACE)
target_include_directories(
  vendor_acutest SYSTEM INTERFACE "$<BUILD_INTERFACE:${vendor_dir}/acutest>"
)

add_library(vendor_copt INTERFACE)
target_include_directories(
  vendor_copt SYSTEM INTERFACE "$<BUILD_INTERFACE:${vendor_dir}/copt>"
)

# mustache4c, tomlc17, and the targets of `libs/src/markup/vendor.cmake` shape every page, so a
# change to the pinned version of one of them, or to a query in `libs/src/markup/queries/`, bumps
# `PAGE_RECIPE_VERSION` in `src/domain/build_cache.h`, which makes every page again.
add_library(vendor_mustache STATIC ${vendor_dir}/mustache4c/mustache.c)
target_include_directories(
  vendor_mustache SYSTEM PUBLIC "$<BUILD_INTERFACE:${vendor_dir}/mustache4c>"
)

add_library(vendor_tomlc17 STATIC ${vendor_dir}/tomlc17/tomlc17.c)
target_include_directories(
  vendor_tomlc17 SYSTEM PUBLIC "$<BUILD_INTERFACE:${vendor_dir}/tomlc17>"
)

foreach(target IN ITEMS vendor_mustache vendor_tomlc17)
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE apps_build_vendor)
endforeach()

# md4c, org4c, and tree-sitter with its grammars, which the `markup` library links.
include("${PROJECT_SOURCE_DIR}/../../libs/src/markup/vendor.cmake")
