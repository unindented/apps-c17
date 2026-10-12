# The private application library contains the app's own code except `app/main.c`. The `sosig`
# executable is built from `app/main.c` and links `os_posix`, the POSIX host from `libs/os`.
# Each unit test links the same library, the host it is registered with, and the unit-test `os_oom`
# from the `testing` library.

configure_file(
  domain/sosig_version.c.in "${CMAKE_CURRENT_BINARY_DIR}/domain/sosig_version.c" @ONLY
)

add_library(sosig_app STATIC "${CMAKE_CURRENT_BINARY_DIR}/domain/sosig_version.c")

add_subdirectory(app)
add_subdirectory(build)
add_subdirectory(domain)

target_link_libraries(
  sosig_app
  PUBLIC vendor_tomlc17
         core
         formats
         markup
         os
         output
         runtime
  PRIVATE apps_build_project
          vendor_copt
          Threads::Threads
)

add_executable(sosig app/main.c)
target_link_libraries(sosig PRIVATE apps_build_project sosig_app os_posix)

if(PROJECT_IS_TOP_LEVEL)
  apps_enable_project_analysis(sosig_app)
  apps_enable_project_analysis(sosig)
endif()
