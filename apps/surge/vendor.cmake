# This file builds the vendored sources that surge uses, from the root `vendor/` directory, and
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

# libgit2, built by upstream's CMake, whose configure checks find the platform's `stat` fields,
# `futimens`, `qsort_r` variant, `iconv` and entropy source. Its options are forced before the
# directory is added, so a cache from another configuration cannot turn on the network
# transports, the tests, the CLI or a shared library. `REGEX_BACKEND=regcomp` uses the libc's POSIX
# regex, which musl and macOS have, so no `pcre2` is vendored.
#
# libgit2 sets `CMAKE_C_STANDARD` as a cache variable (`90`) without `FORCE`, and its old policies
# let that remove a normal variable of the same name in its directory. A cache entry of `17` set
# first stays, so the default is C17 in libgit2's directory and in this project on every configure.
# `apps_build_vendor` requires C17 of every vendored target anyway, so nothing could build as C90.
foreach(option IN ITEMS BUILD_SHARED_LIBS BUILD_TESTS BUILD_CLI BUILD_EXAMPLES BUILD_FUZZERS)
  set(${option} OFF CACHE BOOL "Forced off for surge's vendored libgit2" FORCE)
endforeach()
foreach(option IN ITEMS USE_HTTPS USE_SSH USE_GSSAPI)
  set(${option} OFF CACHE STRING "Forced off for surge's vendored libgit2" FORCE)
endforeach()
set(USE_BUNDLED_ZLIB ON CACHE STRING "Forced on for surge's vendored libgit2" FORCE)
set(USE_THREADS ON CACHE BOOL "Forced on for surge's vendored libgit2" FORCE)
set(REGEX_BACKEND regcomp CACHE STRING "Forced for surge's vendored libgit2" FORCE)
# Its `cmake_minimum_required(VERSION 3.5.1)` predates CMP0077, so its `option()` calls would clear
# this project's normal variables of the same name, such as `CMAKE_C_EXTENSIONS`. The new behavior
# keeps them. CMake still warns that the old minimum is deprecated, which is upstream's to change.
#
# It links the system libraries it finds, such as `rt` on Linux, by name, which this project's
# `CMAKE_LINK_LIBRARIES_ONLY_TARGETS` would reject. It also looks for OpenSSL and GSSAPI whatever
# `USE_HTTPS` and `USE_GSSAPI` say, and a cross build would find the build host's copies, so those
# lookups are turned off.
set(CMAKE_C_STANDARD 17 CACHE STRING "C standard of the targets that set none")
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_LINK_LIBRARIES_ONLY_TARGETS FALSE)
set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_GSSAPI TRUE)
# This project builds as strict C17, and glibc then hides what POSIX 2008 adds, such as the `stat`
# fields `st_mtim` and `st_ctim`, which libgit2's nanosecond check needs. So on Linux its configure
# checks and its sources both get `_DEFAULT_SOURCE`, and agree on what the headers hold. musl
# exposes the same either way, and macOS does not read it.
set(surge_libgit2_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_DEFAULT_SOURCE)
endif()
add_subdirectory(
  "${vendor_dir}/libgit2" "${CMAKE_BINARY_DIR}/vendor/libgit2" EXCLUDE_FROM_ALL SYSTEM
)
set(CMAKE_REQUIRED_DEFINITIONS "${surge_libgit2_required_definitions}")
unset(CMAKE_DISABLE_FIND_PACKAGE_GSSAPI)
unset(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL)
set(CMAKE_LINK_LIBRARIES_ONLY_TARGETS TRUE)
unset(CMAKE_POLICY_DEFAULT_CMP0077)
# Each object library of libgit2 compiles with the vendored-code profile, so the sanitizer builds
# instrument it, and without its warnings. `libgit2package` is the archive of their objects.
foreach(target IN ITEMS libgit2 util xdiff zlib llhttp)
  target_compile_options(${target} PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  target_link_libraries(${target} PRIVATE apps_build_vendor)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE)
  endif()
endforeach()
add_library(vendor_libgit2 INTERFACE)
target_include_directories(
  vendor_libgit2 SYSTEM INTERFACE "$<BUILD_INTERFACE:${vendor_dir}/libgit2/include>"
)
target_link_libraries(vendor_libgit2 INTERFACE libgit2package)
