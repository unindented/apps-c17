# This file builds the vendored sources that cwrap uses, from the root `vendor/` directory, and
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
