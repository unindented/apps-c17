# The private application library contains the app's own code except `app/main.c`. The `surge`
# executable is built from `app/main.c` and links `os_posix`, the POSIX host from `libs/src/os`.
# Each unit test links the same library, the host it is registered with, and the unit-test `os_oom`
# from the `testing` library.

configure_file(
  domain/surge_version.c.in "${CMAKE_CURRENT_BINARY_DIR}/domain/surge_version.c" @ONLY
)

add_library(surge_app STATIC "${CMAKE_CURRENT_BINARY_DIR}/domain/surge_version.c")

add_subdirectory(app)
add_subdirectory(build)
add_subdirectory(domain)
add_subdirectory(vcs)

target_link_libraries(
  surge_app
  PUBLIC vendor_tomlc17
         cli
         core
         formats
         markup
         os
         output
         runtime
         templates
  PRIVATE apps_build_project
          Threads::Threads
)

add_executable(surge app/main.c)
target_link_libraries(surge PRIVATE apps_build_project surge_app os_posix)

if(PROJECT_IS_TOP_LEVEL)
  apps_enable_project_analysis(surge_app)
  apps_enable_project_analysis(surge)
endif()
