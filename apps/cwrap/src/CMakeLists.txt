# The private application library contains project code except `app/main.c`. The `cwrap` executable
# is built from `app/main.c` and links `os_posix`, the POSIX host from `libs/os`. Each unit test
# links the same library, the host it is registered with, and the unit-test `os_oom` from the
# `testing` library.

configure_file(
  app/cwrap_version.c.in "${CMAKE_CURRENT_BINARY_DIR}/app/cwrap_version.c" @ONLY
)

add_library(cwrap_app STATIC "${CMAKE_CURRENT_BINARY_DIR}/app/cwrap_version.c")

add_subdirectory(app)
add_subdirectory(domain)

target_link_libraries(
  cwrap_app
  PUBLIC core os
  PRIVATE apps_build_project vendor_copt
)

add_executable(cwrap app/main.c)
target_link_libraries(cwrap PRIVATE apps_build_project cwrap_app os_posix)

if(PROJECT_IS_TOP_LEVEL)
  apps_enable_project_analysis(cwrap_app)
  apps_enable_project_analysis(cwrap)
endif()
