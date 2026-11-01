#ifndef SURGE_FILETREE_H
#define SURGE_FILETREE_H

#include <stdbool.h>
#include <stddef.h>

#include "core/str.h"
#include "domain/repo_model.h"

struct Arena;
struct Log;
struct Repository;

/**
 * @brief Walks the tree of `branch` into one entry list per directory, and checks every name before
 *        it becomes a path.
 *
 * A gitlink's entry is a row without its object read. A name a page cannot use, `.`, `..`, `.git`
 * in any case, a name that holds `/`, `\`, a control byte, or bytes that are not UTF-8, and a
 * directory named `index.html` in any case, whose tree page would sit below its parent's tree page
 * file, gets a row without a link. So does every name of a directory that collides with another
 * after ASCII case folding, as the Linux kernel's `xt_CONNMARK.h` and `xt_connmark.h` do, with no
 * winner picked, and a name whose pages, `<repo>/blob/<branch>/<path>/index.html` or
 * `<repo>/tree/<branch>/<path>/index.html`, exceed an output-path limit. Each such case is one
 * warning in `log` naming every path involved. A directory without a link is not entered. A
 * symlink's target longer than 4096 bytes is cut there, before any UTF-8 sequence that the cut
 * would split, with a warning.
 *
 * @param tree       Tree that receives the directories, zeroed. Must not be `NULL`.
 * @param repository Open repository. Must not be `NULL`.
 * @param branch     Branch whose commit's tree is walked, which peels to a commit. Must not be
 *                   `NULL`.
 * @param perm       Arena that owns the tree. Must not be `NULL`.
 * @param log        Log that receives the warnings and the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when an object cannot be read.
 */
int filetree_load(struct BranchTree* tree,
                  const struct Repository* repository,
                  const struct Ref* branch,
                  struct Arena* perm,
                  struct Log* log) __attribute__((nonnull(1, 2, 3, 4, 5)));

/**
 * @brief Reports whether an entry has a blob page and may have a raw copy: a linked regular or
 *        executable file.
 *
 * @param entry Entry of a branch's tree. Must not be `NULL`.
 * @return `true` for an entry with a blob page.
 */
bool filetree_entry_has_pages(const struct TreeEntry* entry) __attribute__((nonnull(1)));

#endif
