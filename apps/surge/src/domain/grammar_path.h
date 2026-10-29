#ifndef SURGE_GRAMMAR_PATH_H
#define SURGE_GRAMMAR_PATH_H

#include "core/str.h"

/**
 * @brief Returns the language a file's path names, by its exact base name, such as `Makefile` or
 *        `CMakeLists.txt`, else by its extension in any case, such as `.c` or `.TSX`.
 *
 * The language is a name `highlight_grammar_find` takes. One whose grammar the build does not link
 * finds none there, and the file is written plain. Nothing reads the file's bytes, so the same path
 * always gets the same language.
 *
 * @param path Path of the file in its tree.
 * @return The language, or empty for a path that names none.
 */
struct Str grammar_path_language(struct Str path);

#endif
