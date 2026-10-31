# `cli`: the command line of the three site generators, its parse, its help and version text, and
# its dispatch to the app's `build` and `config` commands, which each app describes in a
# `CliSpec`.

add_library(cli STATIC)
target_sources(cli PRIVATE cli.c cli_dispatch.c cli.h cli_dispatch.h)
libs_add_include_root(cli cli)
target_link_libraries(cli PUBLIC core os runtime PRIVATE apps_build_project vendor_copt Threads::Threads)

if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

apps_enable_project_analysis(cli)

if(NOT libs_build_testing)
  return()
endif()

# The command line prints only through `os/os.h`, so its tests run against the in-memory host.
libs_add_unit_test(cli cli OS memory LIBRARIES testing)
libs_add_unit_test(cli cli_dispatch OS memory LIBRARIES testing)
