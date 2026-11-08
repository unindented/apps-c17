#include "output/output_path.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"

int output_path_check_limits(struct Str relative_path, struct Str source_label, struct Log* log) {
  struct Str segment;
  switch (path_check_output_limits(relative_path, &segment)) {
    case PATH_OUTPUT_OK:
      return 0;
    case PATH_OUTPUT_TOO_LONG:
      return log_error(
          log,
          "output path for '%.*s' exceeds max output path length (%d bytes) at %td bytes: '%.*s'",
          STR_FMT(source_label), OUTPUT_PATH_RELATIVE_LEN_MAX, relative_path.len,
          STR_FMT(relative_path));
    case PATH_OUTPUT_SEGMENT_TOO_LONG:
      return log_error(log,
                       "output path segment for '%.*s' exceeds max filename length (%d bytes) at "
                       "%td bytes: '%.*s'",
                       STR_FMT(source_label), FILENAME_LEN_MAX, segment.len, STR_FMT(segment));
  }
  // Unreachable: the switch covers every `enum PathOutputVerdict`, and omits `default:` so that
  // `-Wswitch` fails the build when a verdict is added without a case here.
  abort();
}

struct Str output_path_to_root(struct Str relative_path, struct Arena* arena) {
  // An accepted path is at most `OUTPUT_PATH_RELATIVE_LEN_MAX` bytes, so `depth * 3` cannot
  // overflow.
  ptrdiff_t depth = 0;
  for (ptrdiff_t i = 0; i < relative_path.len; i++) {
    depth += relative_path.data[i] == '/';
  }
  char* prefix = arena_new(arena, depth * 3, char);
  for (ptrdiff_t i = 0; i < depth; i++) {
    memcpy(prefix + i * 3, "../", 3);
  }
  return (struct Str){prefix, depth * 3};
}

int output_path_check_not_found(struct Str not_found_path, struct Log* log) {
  // `path_is_safe_relative` rejects an empty path too, so an empty value gets the same message.
  if (!path_is_safe_relative(not_found_path) || !str_endswith(not_found_path, S(".html"))) {
    return log_error(log,
                     "config key 'not_found_path' must be a safe relative path ending in '.html': "
                     "'%.*s'",
                     STR_FMT(not_found_path));
  }
  return 0;
}

struct Str output_path_not_found_root(struct Str base_url, struct Arena* arena) {
  // The path starts at the first `/` after the `//` that opens the authority, and an accepted
  // `base_url` holds no `?` or `#` that could end it sooner.
  const struct StrCut scheme = str_cut(base_url, ':');
  const struct Str authority_and_path =
      scheme.is_found ? str_tail(scheme.tail, 2) : (struct Str){0};
  const struct StrCut host = str_cut(authority_and_path, '/');
  const struct Str path =
      host.is_found ? str_tail(authority_and_path, host.head.len) : (struct Str){0};
  return str_concat(path_trim_trailing_slashes(path), S("/"), arena);
}
