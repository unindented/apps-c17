# This file installs the app's executable and creates its release archives. The app's executable
# target and its CPack package have the project's name.

include_guard(GLOBAL)

include(GNUInstallDirs)

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH apps_root_dir)
set(app "${PROJECT_NAME}")

install(TARGETS ${app} RUNTIME COMPONENT ${app}_runtime)
install(
  FILES "${PROJECT_SOURCE_DIR}/README.md"
        "${apps_root_dir}/LICENSE.txt"
        "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
  COMPONENT ${app}_runtime
)

string(TOLOWER "${CMAKE_SYSTEM_NAME}" apps_target_system)
set(APPS_RELEASE_TARGET
    "${CMAKE_SYSTEM_PROCESSOR}-${apps_target_system}"
    CACHE STRING "Platform label in the release archive name"
)

set(CPACK_PACKAGE_NAME "${app}")
set(CPACK_PACKAGE_VENDOR "Daniel Perez Alvarez")
set(CPACK_PACKAGE_CONTACT "${CPACK_PACKAGE_VENDOR} <daniel@unindented.org>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_RESOURCE_FILE_LICENSE "${apps_root_dir}/LICENSE.txt")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CPACK_PACKAGE_NAME}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${APPS_RELEASE_TARGET}")
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}")
set(CPACK_STRIP_FILES TRUE)
set(CPACK_GENERATOR "TGZ")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-source")

# The source archive holds the whole repository except the other apps, because the app builds with
# the root CMake modules, the libraries in `libs/` and the sources in `vendor/`. CPack includes
# untracked files when it archives the source tree. Keep the default exclusions. Also exclude build
# products and the files that stay on one machine.
set(CPACK_SOURCE_INSTALLED_DIRECTORIES "${apps_root_dir};/")
set(CPACK_SOURCE_IGNORE_FILES
    "/CVS/"
    "/\\.svn/"
    "/\\.bzr/"
    "/\\.hg/"
    "/\\.git/"
    "\\.swp$"
    "\\.#"
    "/#"
    "^${apps_root_dir}/build/"
    "^${apps_root_dir}/apps/[^/]+/build/"
    "^${apps_root_dir}/libs/build/"
    "/CMakeUserPresets\\.json$"
)
file(GLOB apps_dirs LIST_DIRECTORIES TRUE "${apps_root_dir}/apps/*")
foreach(apps_dir IN LISTS apps_dirs)
  cmake_path(GET apps_dir FILENAME apps_dir_name)
  if(NOT apps_dir_name STREQUAL app)
    list(APPEND CPACK_SOURCE_IGNORE_FILES "^${apps_root_dir}/apps/${apps_dir_name}/")
  endif()
endforeach()
set(CPACK_VERBATIM_VARIABLES TRUE)
# A binary archive installs only the app's runtime component. A vendored project that upstream's
# CMake builds, such as surge's libgit2, has `install()` rules with no component, which would put
# its static library and headers in the archive.
set(CPACK_INSTALL_CMAKE_PROJECTS "${PROJECT_BINARY_DIR};${app};${app}_runtime;/")

include(CPack)
