# The private application library contains the app's own code except `app/main.c`. The `fram`
# executable is built from `app/main.c` and links `os_posix`, the POSIX host from `libs/runtime`.
# Each unit test links the same library, the host it is registered with, and the unit-test `os_oom`
# from the `testing` library.

configure_file(
  core/fram_version.c.in "${CMAKE_CURRENT_BINARY_DIR}/core/fram_version.c" @ONLY
)

add_library(fram_app STATIC "${CMAKE_CURRENT_BINARY_DIR}/core/fram_version.c")

add_subdirectory(app)
add_subdirectory(build)
add_subdirectory(core)
add_subdirectory(domain)
add_subdirectory(formats)
add_subdirectory(runtime)

target_link_libraries(
  fram_app
  PUBLIC vendor_tomlc17
         core
         formats
         runtime
         shared
  PRIVATE apps_build_project
          vendor_copt
          Threads::Threads
)

add_executable(fram app/main.c)
target_link_libraries(fram PRIVATE apps_build_project fram_app os_posix)

if(PROJECT_IS_TOP_LEVEL)
  apps_enable_project_analysis(fram_app)
  apps_enable_project_analysis(fram)
endif()
