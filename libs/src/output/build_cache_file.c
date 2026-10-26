#include "output/build_cache_file.h"

#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "os/os.h"

int build_cache_file_parse(struct Str text,
                           struct Str header,
                           struct Str header_mismatch,
                           BuildCacheFileBeginFn begin,
                           BuildCacheFileRowFn parse_row,
                           void* userdata,
                           struct Arena* perm,
                           struct Log* log) {
  if (text.len == 0) {
    return log_error(log, "build cache is empty, as a build that did not finish leaves it");
  }
  if (!str_startswith(text, header)) {
    return log_error(log, "%.*s", STR_FMT(header_mismatch));
  }
  ptrdiff_t header_line_count = 0;
  for (ptrdiff_t i = 0; i < header.len; i++) {
    header_line_count += header.data[i] == '\n';
  }
  const struct Str rows = str_tail(text, header.len);
  ptrdiff_t row_count = 0;
  for (ptrdiff_t i = 0; i < rows.len; i++) {
    row_count += rows.data[i] == '\n';
  }
  // The rows go into a copy of `perm`, which only a parse that succeeds hands back.
  struct Arena arena = *perm;
  begin(row_count, &arena, userdata);
  ptrdiff_t index = 0;
  // A file cut short by an interrupted write ends without its last newline, which is the reason
  // every row must end in one.
  struct StrCut cut = str_cut(rows, '\n');
  for (; cut.is_found; cut = str_cut(cut.tail, '\n')) {
    if (parse_row(cut.head, index, &arena, userdata) != 0) {
      return log_error(log, "build cache does not parse at line %td",
                       header_line_count + index + 1);
    }
    index++;
  }
  if (cut.head.len > 0) {
    return log_error(log, "build cache does not parse at line %td", header_line_count + index + 1);
  }
  *perm = arena;
  return 0;
}

enum BuildCacheFileState build_cache_file_load(struct Str path,
                                               ptrdiff_t file_len_max,
                                               BuildCacheFileParseFn parse,
                                               void* userdata,
                                               struct Os* os,
                                               struct Arena* perm,
                                               struct Arena scratch,
                                               struct Str* reason_out) {
  // `os_stat` finds a missing build cache without a message, which is the usual case on a first
  // build.
  struct OsStat st;
  if (os_stat(os, path, &st) != 0) {
    return BUILD_CACHE_FILE_MISSING;
  }
  // The reason grows in the scratch, which neither the read nor the parse receives, and is copied
  // into `perm` once it is whole.
  struct Log cache_log = {.buf = {.arena = &scratch}};
  struct Str text = {0};
  if (os_read(os, path, file_len_max, OS_READ_TEXT, perm, &text, S("failed to read build cache"),
              &cache_log) != 0 ||
      parse(text, perm, &cache_log, userdata) != 0) {
    // Each reason is one line, whose newline the note leaves out.
    const struct Str logged = buf_str(&cache_log.buf);
    *reason_out =
        str_clone(str_endswith(logged, S("\n")) ? str_head(logged, logged.len - 1) : logged, perm);
    return BUILD_CACHE_FILE_UNUSABLE;
  }
  return BUILD_CACHE_FILE_LOADED;
}
