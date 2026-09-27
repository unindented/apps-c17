# This file defines the vendored targets that the libraries and their tests use, from the root
# `vendor/` directory, when this project is configured on its own. An app that adds this project
# defines these targets in its own `vendor.cmake`.

cmake_path(SET vendor_dir NORMALIZE "${PROJECT_SOURCE_DIR}/../vendor")

add_library(vendor_acutest INTERFACE)
target_include_directories(
  vendor_acutest SYSTEM INTERFACE "$<BUILD_INTERFACE:${vendor_dir}/acutest>"
)

add_library(vendor_tomlc17 STATIC "${vendor_dir}/tomlc17/tomlc17.c")
target_include_directories(
  vendor_tomlc17 SYSTEM PUBLIC "$<BUILD_INTERFACE:${vendor_dir}/tomlc17>"
)
target_compile_options(vendor_tomlc17 PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
target_link_libraries(vendor_tomlc17 PRIVATE apps_build_vendor)
