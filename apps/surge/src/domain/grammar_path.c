#include "domain/grammar_path.h"

#include <stddef.h>

#include "core/path.h"
#include "core/str.h"
#include "core/text.h"

/** A file name or an extension, and the language it names. */
struct GrammarPathRule {
  /** The exact base name, or the extension with its dot. */
  struct Str match;

  /** The language, as `highlight_grammar_find` takes it. */
  struct Str language;
};

/** Base names that name a language whatever their extension, compared exactly. */
static const struct GrammarPathRule GRAMMAR_PATH_NAMES[] = {
    {STR_INIT("CMakeLists.txt"), STR_INIT("cmake")},
    {STR_INIT("Dockerfile"), STR_INIT("dockerfile")},
    {STR_INIT("GNUmakefile"), STR_INIT("make")},
    {STR_INIT("Makefile"), STR_INIT("make")},
    {STR_INIT("makefile"), STR_INIT("make")},
};

/** Extensions that name a language, compared after ASCII case folding. */
static const struct GrammarPathRule GRAMMAR_PATH_EXTENSIONS[] = {
    {STR_INIT(".bash"), STR_INIT("bash")},
    {STR_INIT(".c"), STR_INIT("c")},
    {STR_INIT(".cjs"), STR_INIT("javascript")},
    {STR_INIT(".cmake"), STR_INIT("cmake")},
    {STR_INIT(".css"), STR_INIT("css")},
    {STR_INIT(".cts"), STR_INIT("typescript")},
    {STR_INIT(".h"), STR_INIT("c")},
    {STR_INIT(".htm"), STR_INIT("html")},
    {STR_INIT(".html"), STR_INIT("html")},
    {STR_INIT(".js"), STR_INIT("javascript")},
    {STR_INIT(".json"), STR_INIT("json")},
    {STR_INIT(".jsx"), STR_INIT("javascript")},
    {STR_INIT(".ksh"), STR_INIT("bash")},
    {STR_INIT(".mjs"), STR_INIT("javascript")},
    {STR_INIT(".mk"), STR_INIT("make")},
    {STR_INIT(".mts"), STR_INIT("typescript")},
    {STR_INIT(".sh"), STR_INIT("bash")},
    {STR_INIT(".ts"), STR_INIT("typescript")},
    {STR_INIT(".tsx"), STR_INIT("tsx")},
    {STR_INIT(".zsh"), STR_INIT("bash")},
};

struct Str grammar_path_language(struct Str path) {
  const struct Str name = path_basename(path);
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(GRAMMAR_PATH_NAMES) / sizeof(GRAMMAR_PATH_NAMES[0]));
       i++) {
    if (str_eq(name, GRAMMAR_PATH_NAMES[i].match)) {
      return GRAMMAR_PATH_NAMES[i].language;
    }
  }
  // A name that is only an extension, such as `.c`, has no base to name a language.
  for (ptrdiff_t i = 0;
       i < (ptrdiff_t)(sizeof(GRAMMAR_PATH_EXTENSIONS) / sizeof(GRAMMAR_PATH_EXTENSIONS[0])); i++) {
    const struct Str extension = GRAMMAR_PATH_EXTENSIONS[i].match;
    if (name.len > extension.len && text_has_suffix_folded(name, extension)) {
      return GRAMMAR_PATH_EXTENSIONS[i].language;
    }
  }
  return (struct Str){0};
}
