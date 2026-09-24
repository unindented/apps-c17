# This file defines the vendored targets that the libraries' tests use, from the root `vendor/`
# directory, when this project is configured on its own. An app that adds this project defines
# these targets in its own `vendor.cmake`.

cmake_path(SET vendor_dir NORMALIZE "${PROJECT_SOURCE_DIR}/../vendor")

add_library(vendor_acutest INTERFACE)
target_include_directories(
  vendor_acutest SYSTEM INTERFACE "$<BUILD_INTERFACE:${vendor_dir}/acutest>"
)
