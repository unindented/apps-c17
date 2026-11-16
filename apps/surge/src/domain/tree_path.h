#ifndef SURGE_TREE_PATH_H
#define SURGE_TREE_PATH_H

#include <stdbool.h>

#include "core/str.h"

struct Arena;
struct BranchTree;
struct TreeEntry;

/**
 * @brief Resolves a relative or `/`-rooted path against `dir_path`, with its `.` and `..` segments
 *        applied and its empty segments dropped.
 *
 * @param dir_path Directory the path is relative to, empty for the root.
 * @param path     Path to resolve, as its bytes are.
 * @param arena    Arena that owns the result. Must not be `NULL`.
 * @param path_out Receives the path from the root, empty for the root itself. Must not be `NULL`.
 * @return `true` when the path stays inside the tree.
 */
bool tree_path_resolve(struct Str dir_path,
                       struct Str path,
                       struct Arena* arena,
                       struct Str* path_out) __attribute__((nonnull(3, 4)));

/**
 * @brief Returns the directory of a path in a tree, empty for a path in the root.
 *
 * @param path Path from the root, not empty.
 * @return A view of `path` before its last `/`.
 */
struct Str tree_path_dir(struct Str path);

/**
 * @brief Finds the linked file at `path` of a branch's tree.
 *
 * @param tree Tree of the branch. Must not be `NULL`.
 * @param path Path from the root.
 * @return The file's entry, or `NULL` when the tree has no linked file there.
 */
const struct TreeEntry* tree_path_find_file(const struct BranchTree* tree, struct Str path)
    __attribute__((nonnull(1)));

/**
 * @brief Finds the symlink at `path` of a branch's tree, in a directory with a tree page.
 *
 * @param tree Tree of the branch. Must not be `NULL`.
 * @param path Path from the root.
 * @return The symlink's entry, or `NULL` when the tree has no such symlink there.
 */
const struct TreeEntry* tree_path_find_symlink(const struct BranchTree* tree, struct Str path)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether `path` names a directory of a branch's tree with a tree page.
 *
 * @param tree Tree of the branch. Must not be `NULL`.
 * @param path Path from the root, empty for the root, which always has one.
 * @return `true` for such a directory.
 */
bool tree_path_has_dir(const struct BranchTree* tree, struct Str path) __attribute__((nonnull(1)));

#endif
