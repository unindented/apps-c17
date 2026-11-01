# This script makes surge's fixture repositories from the `git fast-import` streams in
# `SURGE_FIXTURE_STREAMS_DIR`, one directory per stream in `SURGE_FIXTURE_REPOS_DIR`, replacing
# what is there. Every stream fixes its dates, so every object id is the same on every run. git
# runs with `GIT_CONFIG_NOSYSTEM=1` and a `HOME` of its own below `SURGE_FIXTURE_REPOS_DIR`, so no
# config of the machine changes what it makes.
#
# The streams hold what `git fast-import` accepts. This script adds what it refuses:
#
# - `showcase`: a working copy, checked out, with a nested tag (a tag of the tag `v0.2.0`) and a
#   tag of a tree.
# - `hostile`: tree entries named `..`, `.GIT` and one with a control byte, which only
#   `git mktree` writes, in a commit on top of the stream's, and a signed commit on top of that,
#   which the tag `signed` names.
# - `empty`: a repository with no commit.
# - `branch-case`: branches whose names, or the directories above them, differ only in case,
#   `Main` and `main`, `a` and `A/b`, and `X/x` and `x/y`, and a branch whose name is Latin-1, not
#   UTF-8, in a `packed-refs` that a case-insensitive or UTF-8-only filesystem could not hold as
#   loose refs.
# - `ref-prefix`: a branch `feature` next to `feature/x`, which only a hand-written `packed-refs`
#   can hold.
#
# Every repository but `showcase` is bare.

foreach(required IN ITEMS SURGE_GIT SURGE_FIXTURE_STREAMS_DIR SURGE_FIXTURE_REPOS_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

set(fixture_home "${SURGE_FIXTURE_REPOS_DIR}/.home")
set(ENV{GIT_CONFIG_NOSYSTEM} 1)
set(ENV{HOME} "${fixture_home}")
set(ENV{XDG_CONFIG_HOME} "${fixture_home}/.config")
# Nothing of the environment chooses the repository, the config, the hash or the ref storage.
foreach(variable IN ITEMS GIT_DIR GIT_WORK_TREE GIT_CONFIG_GLOBAL GIT_CONFIG_PARAMETERS
                          GIT_DEFAULT_HASH GIT_DEFAULT_REF_FORMAT
)
  unset(ENV{${variable}})
endforeach()
# The objects that this script writes itself get the same author, committer and dates on every run.
set(ENV{GIT_AUTHOR_NAME} "Ada Lovelace")
set(ENV{GIT_AUTHOR_EMAIL} "ada@example.org")
set(ENV{GIT_AUTHOR_DATE} "1767956400 +0100")
set(ENV{GIT_COMMITTER_NAME} "Ada Lovelace")
set(ENV{GIT_COMMITTER_EMAIL} "ada@example.org")
set(ENV{GIT_COMMITTER_DATE} "1767956400 +0100")

# Runs git in `dir` with the arguments that follow, and stops the script when it fails. The
# standard output, stripped, lands in `out_var` when `OUTPUT` names one, and `INPUT` names a file
# that git reads as its standard input.
function(fixture_git dir)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "OUTPUT;INPUT" "")
  set(input_args)
  if(arg_INPUT)
    set(input_args INPUT_FILE "${arg_INPUT}")
  endif()
  execute_process(
    COMMAND "${SURGE_GIT}" ${arg_UNPARSED_ARGUMENTS}
    WORKING_DIRECTORY "${dir}" ${input_args}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT result EQUAL 0)
    list(JOIN arg_UNPARSED_ARGUMENTS " " command)
    message(FATAL_ERROR "git ${command} failed in '${dir}' with status ${result}:\n${error}")
  endif()
  if(arg_OUTPUT)
    set(${arg_OUTPUT} "${output}" PARENT_SCOPE)
  endif()
endfunction()

# Makes the repository `name` from its stream, bare unless `IS_WORKING_COPY` is given, and sets
# `git_dir_var` to its git directory.
function(fixture_import name git_dir_var)
  cmake_parse_arguments(PARSE_ARGV 2 arg "IS_WORKING_COPY" "" "")
  set(repo_dir "${SURGE_FIXTURE_REPOS_DIR}/${name}")
  # The object ids of `tests/repos/<name>.refs` are SHA-1, and the refusal fixtures write
  # `packed-refs` by hand, so the hash and the ref storage are pinned. A git older than 2.45, which
  # has no reftable, ignores the setting and uses files.
  set(init_args init --quiet --object-format=sha1 --initial-branch=main)
  if(arg_IS_WORKING_COPY)
    set(git_dir "${repo_dir}/.git")
  else()
    list(APPEND init_args --bare)
    set(git_dir "${repo_dir}")
  endif()
  fixture_git("${SURGE_FIXTURE_REPOS_DIR}" -c init.defaultRefFormat=files ${init_args} "${repo_dir}")
  # git sets `core.ignorecase` on a case-insensitive filesystem, and fast-import then folds the
  # hostile fixture's case-colliding paths into one, so every host keeps them apart.
  fixture_git("${git_dir}" config core.ignorecase false)
  set(stream "${SURGE_FIXTURE_STREAMS_DIR}/${name}.fi")
  if(EXISTS "${stream}")
    fixture_git("${git_dir}" fast-import --quiet INPUT "${stream}")
  endif()
  set(${git_dir_var} "${git_dir}" PARENT_SCOPE)
endfunction()

# Writes `text` to a scratch file and runs `git mktag` on it in `git_dir`, then points `ref` at the
# new tag.
function(fixture_mktag git_dir ref text)
  set(tag_file "${fixture_home}/tag.txt")
  file(WRITE "${tag_file}" "${text}")
  fixture_git("${git_dir}" mktag INPUT "${tag_file}" OUTPUT tag_oid)
  fixture_git("${git_dir}" update-ref "${ref}" "${tag_oid}")
endfunction()

# Replaces the loose and packed refs of `git_dir` with a `packed-refs` that points every ref in
# `refs` at the commit of `main`, so it can hold refs that a filesystem or git itself refuses.
function(fixture_pack_refs git_dir)
  fixture_git("${git_dir}" rev-parse refs/heads/main OUTPUT main_oid)
  file(REMOVE_RECURSE "${git_dir}/refs/heads")
  file(MAKE_DIRECTORY "${git_dir}/refs/heads")
  set(packed "# pack-refs with: peeled fully-peeled sorted \n")
  foreach(ref IN LISTS ARGN)
    string(APPEND packed "${main_oid} ${ref}\n")
  endforeach()
  file(WRITE "${git_dir}/packed-refs" "${packed}")
endfunction()

file(REMOVE_RECURSE "${SURGE_FIXTURE_REPOS_DIR}")
file(MAKE_DIRECTORY "${fixture_home}")

fixture_import(showcase showcase_git_dir IS_WORKING_COPY)
fixture_git("${showcase_git_dir}/.." reset --quiet --hard)
fixture_git("${showcase_git_dir}" rev-parse refs/tags/v0.2.0 OUTPUT release_tag_oid)
fixture_mktag(
  "${showcase_git_dir}" refs/tags/v0.2.0-nested
  "object ${release_tag_oid}\ntype tag\ntag v0.2.0-nested\ntagger Ada Lovelace <ada@example.org> 1767956400 +0100\n\nA tag of the tag v0.2.0.\n"
)
fixture_git("${showcase_git_dir}" rev-parse "refs/heads/main^{tree}" OUTPUT main_tree_oid)
fixture_mktag(
  "${showcase_git_dir}" refs/tags/tree-tag
  "object ${main_tree_oid}\ntype tree\ntag tree-tag\ntagger Ada Lovelace <ada@example.org> 1767956400 +0100\n\nA tag of the tree of main.\n"
)

fixture_import(monorepo monorepo_git_dir)
# The config gives the monorepo no description, so the scan reads this one. It is longer than the
# path it is read from on a usual checkout, and it ends in the newline the scan trims.
file(
  WRITE "${monorepo_git_dir}/description"
  "Two projects in one repository, described by its own description file rather than by the config, in more bytes than any path the file is read from, with an ñ in UTF-8.\n"
)

# Each hostile entry is a blob next to the stream's entries, in a tree that `git mktree` writes from
# `git ls-tree` output, so git checks none of the names.
fixture_import(hostile hostile_git_dir)
fixture_git("${hostile_git_dir}" ls-tree refs/heads/main OUTPUT hostile_entries)
fixture_git("${hostile_git_dir}" rev-parse "refs/heads/main:ok.txt" OUTPUT hostile_blob_oid)
fixture_git("${hostile_git_dir}" rev-parse "refs/heads/main:include" OUTPUT include_tree_oid)
string(ASCII 1 control_byte)
string(ASCII 255 invalid_byte)
set(hostile_listing "${hostile_entries}\n")
foreach(
  hostile_name IN
  ITEMS ".."
        ".GIT"
        "ctl${control_byte}name"
        "back\\slash"
        "bad${invalid_byte}utf8"
)
  string(APPEND hostile_listing "100644 blob ${hostile_blob_oid}\t${hostile_name}\n")
endforeach()
# A directory named `index.html` would put its tree page below its parent's page file.
string(APPEND hostile_listing "040000 tree ${include_tree_oid}\tindex.html\n")
# Four directories of 250-byte names: the tree page of the fourth passes the output path limit.
file(WRITE "${fixture_home}/deep.txt" "100644 blob ${hostile_blob_oid}\tok.txt\n")
fixture_git("${hostile_git_dir}" mktree INPUT "${fixture_home}/deep.txt" OUTPUT deep_tree_oid)
foreach(letter IN ITEMS d c b a)
  string(REPEAT "${letter}" 250 deep_name)
  file(WRITE "${fixture_home}/deep.txt" "040000 tree ${deep_tree_oid}\t${deep_name}\n")
  fixture_git("${hostile_git_dir}" mktree INPUT "${fixture_home}/deep.txt" OUTPUT deep_tree_oid)
endforeach()
string(APPEND hostile_listing "040000 tree ${deep_tree_oid}\t${deep_name}\n")
file(WRITE "${fixture_home}/hostile-tree.txt" "${hostile_listing}")
fixture_git(
  "${hostile_git_dir}" mktree INPUT "${fixture_home}/hostile-tree.txt" OUTPUT hostile_tree_oid
)
# Someone else commits this one, later than its author wrote it, so a page that shows the author and
# the committer tells them apart.
set(ENV{GIT_COMMITTER_NAME} "Grace Hopper")
set(ENV{GIT_COMMITTER_EMAIL} "grace@example.org")
set(ENV{GIT_COMMITTER_DATE} "1767999600 +0000")
fixture_git(
  "${hostile_git_dir}" commit-tree "${hostile_tree_oid}" -p refs/heads/main -m
  "Add names that git refuses" -m "Each of them is a blob next to the stream's entries."
  OUTPUT hostile_commit_oid
)
set(ENV{GIT_COMMITTER_NAME} "Ada Lovelace")
set(ENV{GIT_COMMITTER_EMAIL} "ada@example.org")
set(ENV{GIT_COMMITTER_DATE} "1767956400 +0100")
fixture_git("${hostile_git_dir}" update-ref refs/heads/main "${hostile_commit_oid}")
# A signed commit on top, which the tag `signed` names. Its `gpgsig` header holds an SSH signature
# that signs nothing, since surge shows a signature and checks none. `git hash-object` writes it as
# it is, where `git commit-tree` would need a key and `git fast-import` a recent git.
file(
  WRITE "${fixture_home}/signed-commit.txt"
  "tree ${hostile_tree_oid}\nparent ${hostile_commit_oid}\nauthor Ada Lovelace <ada@example.org> 1768003200 +0000\ncommitter Ada Lovelace <ada@example.org> 1768003200 +0000\ngpgsig -----BEGIN SSH SIGNATURE-----\n U1NIU0lHAAAAAQAAADMAAAALc3NoLWVkMjU1MTkAAAAgc3VyZ2UgZml4dHVyZQ==\n -----END SSH SIGNATURE-----\n\nSign the names that git refuses\n"
)
fixture_git(
  "${hostile_git_dir}" hash-object -t commit -w "${fixture_home}/signed-commit.txt"
  OUTPUT signed_commit_oid
)
fixture_git("${hostile_git_dir}" update-ref refs/tags/signed "${signed_commit_oid}")
# A branch can name a tree, as a tag can, and gets no pages then. git refuses to write one, so the
# loose ref is written by hand.
file(WRITE "${hostile_git_dir}/refs/heads/tree-branch" "${include_tree_oid}\n")

fixture_import(empty empty_git_dir)
# The config sets the empty repository's description to an empty string, which hides this file, so
# no page may show its text.
file(WRITE "${empty_git_dir}/description" "Hidden by the config's empty description.\n")

# A deletion, a file that becomes a symlink, and a change of two hunks, which only the unit tests of
# `changeset` read.
fixture_import(changes changes_git_dir)

fixture_import(branch-case branch_case_git_dir)
string(ASCII 233 latin1_e_acute)
fixture_pack_refs(
  "${branch_case_git_dir}"
  refs/heads/A/b
  refs/heads/Main
  refs/heads/X/x
  refs/heads/a
  "refs/heads/caf${latin1_e_acute}"
  refs/heads/main
  refs/heads/x/y
)

fixture_import(ref-prefix ref_prefix_git_dir)
fixture_pack_refs(
  "${ref_prefix_git_dir}" refs/heads/feature refs/heads/feature/x refs/heads/main
)

file(REMOVE_RECURSE "${fixture_home}")
