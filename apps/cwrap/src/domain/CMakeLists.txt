target_sources(
  cwrap_app
  PRIVATE atom.c comment.c doxygen.c fill.c lex.c rewrite.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES atom.h comment.h cwrap_version.h doxygen.h fill.h lex.h rewrite.h
               unicode_width_data.h
)

if(APPS_BUILD_TESTING)
  foreach(module IN ITEMS atom comment doxygen fill lex rewrite)
    apps_add_unit_test(${module})
  endforeach()
endif()
