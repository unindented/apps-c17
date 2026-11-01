#include "runtime/read_root.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/error.h"
#include "core/log.h"
#include "core/str.h"
#include "os/os.h"

int read_root_require(const struct ReadRoot* roots,
                      ptrdiff_t root_count,
                      struct Os* os,
                      struct Log* log) {
  int rc = 0;
  for (ptrdiff_t i = 0; i < root_count; i++) {
    const struct ReadRoot* root = &roots[i];
    // A root gets two questions in order, because the second presupposes the first answer.
    //
    // Is it there at all? Only an optional root may answer no, and for that root absence is the
    // expected case rather than a failure, so it is usable without a message. Only `ENOENT` answers
    // no. A root that cannot be inspected, such as one below a directory the user may not search,
    // may well be there, so it is reported like any other.
    //
    // Is it usable? The message carries the cause. `ENOENT` and `EACCES` call for different fixes,
    // and nothing after this point would report either, because no caller walks an unusable root.
    // The cause carries no verb of its own, so the operation names what was being resolved, as in
    // `failed to resolve config directory 'templates_dir': Permission denied ('templates')`.
    struct OsStat st;
    if (os_stat(os, root->dir_path, &st) != 0) {
      if (root->is_optional && errno == ENOENT) {
        continue;
      }
      char message[ERROR_SYSTEM_MESSAGE_SIZE];
      rc = log_error(log, "failed to resolve config directory '%.*s': %s ('%.*s')",
                     STR_FMT(root->key), error_system_message(message, sizeof(message), errno),
                     STR_FMT(root->dir_path));
    } else if (!st.is_dir) {
      rc = log_error(log, "failed to resolve config directory '%.*s': not a directory ('%.*s')",
                     STR_FMT(root->key), STR_FMT(root->dir_path));
    }
  }
  return rc;
}
