# This script builds one copied fixture site three times, over fixture repositories it makes from
# their `git fast-import` streams. It checks that the second build, which reuses the output cache,
# and a third, `--force` build, which ignores it, each leave the output unchanged, then compares
# the result with the expected output. The release workflow runs it against a packaged binary, with
# `SURGE_EMULATOR` naming a launcher such as `qemu-aarch64` when the binary targets another
# architecture.
#
# The comparison leaves out the output cache, `_surge/output-cache.tsv`, whose rows name a page
# recipe that hashes the modification times of the copied templates. The packs are compared byte for
# byte too: libgit2 writes the same pack for the same objects on one thread. Each pack is also
# checked with `git verify-pack` and a strict `git index-pack`, and each `info/refs` against
# `git for-each-ref` of its source. Before the comparison, a dry run must keep every output. After
# it, a config that renames two templates in a `[templates]` table must make every page again and
# give the same output. Then a commit moves a branch: a dry run must write only what the move decides, and after a build
# that fails at a page, the next two builds must keep the previous pack one build. Then limits that
# give a blob a raw copy but no shown page, and a `base_url` with a path, check that copy and the
# paths of `_headers`, and last a changed blob must make only what it decides again. The first
# build's `stderr` must be the warnings in `<expected>.warnings.txt`, or empty without that file.
#
# An expected file's `@PROJECT_VERSION@` stands for `SURGE_VERSION`, as in fram's golden test.
#
# With `SURGE_EXPECTED_STDERR`, the site is one that a build must refuse: the script runs one
# verbose build on one worker, which must fail with exactly that file's text on `stderr`, its status
# lines included, so the file shows which phases ran before the build stopped, and create no
# `public/`. With `SURGE_PYTHON`, it then clones every clone export over dumb HTTP with
# `tests/clone_test.py`, and with `SURGE_MISE` as well, installs the monorepo's release project with
# `tests/mise_test.py`. The steps that sosig's and fram's golden tests share are in
# `cmake/apps_golden.cmake`.

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/apps_golden.cmake")

foreach(
  required IN
  ITEMS SURGE_EXECUTABLE
        SURGE_GIT
        SURGE_SITE_DIR
        SURGE_EXPECTED_DIR
        SURGE_SCRATCH_DIR
        SURGE_FIXTURE_STREAMS_DIR
        SURGE_VERSION
)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${SURGE_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SURGE_SCRATCH_DIR}")
file(COPY "${SURGE_SITE_DIR}/" DESTINATION "${SURGE_SCRATCH_DIR}")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" "-DSURGE_GIT=${SURGE_GIT}"
    "-DSURGE_FIXTURE_STREAMS_DIR=${SURGE_FIXTURE_STREAMS_DIR}"
    "-DSURGE_FIXTURE_REPOS_DIR=${SURGE_SCRATCH_DIR}/repos" -P
    "${CMAKE_CURRENT_LIST_DIR}/surge_make_fixture_repos.cmake"
  RESULT_VARIABLE make_result
  ERROR_VARIABLE make_error
)
if(NOT make_result EQUAL 0)
  message(FATAL_ERROR "making the fixture repositories failed:\n${make_error}")
endif()

set(actual_dir "${SURGE_SCRATCH_DIR}/public")
set(first_dir "${SURGE_SCRATCH_DIR}/first/public")

if(DEFINED SURGE_EXPECTED_STDERR)
  execute_process(
    COMMAND ${SURGE_EMULATOR} "${SURGE_EXECUTABLE}" build --verbose --workers 1
    WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
  )
  file(READ "${SURGE_EXPECTED_STDERR}" expected_stderr)
  if(NOT build_result EQUAL 1 OR NOT build_stderr STREQUAL expected_stderr)
    message(
      FATAL_ERROR "the build of a site it must refuse exited with ${build_result}:\n"
                  "${build_stdout}${build_stderr}"
    )
  endif()
  if(EXISTS "${actual_dir}")
    message(FATAL_ERROR "the refused build created '${actual_dir}'")
  endif()
  return()
endif()

# A stale page and a stale directory tree sit in the output before the first build, as an earlier
# build of other repos would leave them, and so does a stale pack. The build removes all of them.
file(WRITE "${actual_dir}/stale.html" "stale")
file(WRITE "${actual_dir}/stale-dir/nested/stale.txt" "stale")
file(WRITE "${actual_dir}/showcase.git/objects/pack/pack-stale.pack" "stale")

set(ENV{TMPDIR} "${SURGE_SCRATCH_DIR}/tmp")
file(MAKE_DIRECTORY "$ENV{TMPDIR}")

# The first build runs one worker and the forced third eight, so every output is made with both and
# must come out the same. The second runs verbose, so its status lines show it loaded the build
# cache and used it.
foreach(build IN ITEMS first second third)
  set(build_args --workers 2 --verbose)
  if(build STREQUAL "first")
    set(build_args --workers 1)
  elseif(build STREQUAL "third")
    set(build_args --workers 8 --force)
  endif()
  apps_golden_run_build(
    EXECUTABLE "${SURGE_EXECUTABLE}"
    EMULATOR ${SURGE_EMULATOR}
    WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
    LABEL "${build} fixture site"
    ARGS ${build_args}
    STDERR build_stderr
  )
  if(build STREQUAL "first")
    # The warnings of a site that builds are pinned next to its expected output.
    set(expected_warnings "")
    if(EXISTS "${SURGE_EXPECTED_DIR}.warnings.txt")
      file(READ "${SURGE_EXPECTED_DIR}.warnings.txt" expected_warnings)
    endif()
    if(NOT build_stderr STREQUAL expected_warnings)
      message(FATAL_ERROR "the first build's warnings differ from expected:\n${build_stderr}")
    endif()
    file(COPY "${actual_dir}" DESTINATION "${SURGE_SCRATCH_DIR}/first")
    continue()
  endif()
  if(build STREQUAL "second"
     AND NOT build_stderr MATCHES "\ncompiling templates\nloading output cache\nscanning repos, "
  )
    message(FATAL_ERROR "the second build did not load and use the output cache:\n${build_stderr}")
  endif()
  # The output cache holds the times of the copied templates, so it may change.
  apps_golden_check_unchanged(
    FIRST_DIR "${first_dir}" ACTUAL_DIR "${actual_dir}" LABEL "${build}" IGNORE "^_surge/"
  )
endforeach()

# A dry run after the builds keeps every output, the packs included, and would write only the
# output cache, which every build writes again. It changes nothing, which the comparison below shows.
apps_golden_run_build(
  EXECUTABLE "${SURGE_EXECUTABLE}"
  EMULATOR ${SURGE_EMULATOR}
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
  LABEL "dry run of the fixture site"
  ARGS --dry-run
  STDOUT dry_run_stdout
)
apps_golden_check_dry_run_keeps(
  LISTING "${dry_run_stdout}" CACHE_PATH "public/_surge/output-cache.tsv"
)
if(NOT dry_run_stdout MATCHES "\nkeep\tpublic/showcase\.git/objects/pack/pack-[0-9a-f]+\.pack\n")
  message(FATAL_ERROR "the dry run does not keep the clone pack:\n${dry_run_stdout}")
endif()

# The packs the build wrote below `TMPDIR` are gone once it returns.
file(GLOB leftover_temp "$ENV{TMPDIR}/*")
if(leftover_temp)
  message(FATAL_ERROR "the build left temporary files: '${leftover_temp}'")
endif()

# Only text files carry the version placeholder, and a pack must stay byte for byte as it is.
apps_golden_compare_trees(
  EXPECTED_DIR "${SURGE_EXPECTED_DIR}"
  ACTUAL_DIR "${actual_dir}"
  SCRATCH_DIR "${SURGE_SCRATCH_DIR}"
  VERSION "${SURGE_VERSION}"
  VERSIONED "\\.(html|xml|atom|txt|json)$"
  EXCLUDE "_surge/output-cache.tsv"
)

# A second config renames `tree.html` and `commit.html` and names them in a `[templates]` table. The
# page recipe holds every template name, so a dry run makes every page again, and the build after it
# writes the same pages. The templates and the config are put back afterwards, and a build settles
# the output cache on the first config's recipe for the steps below.
set(templates_config_file "${SURGE_SCRATCH_DIR}/surge.toml")
file(READ "${templates_config_file}" templates_config_text)
set(templates_dir "${SURGE_SCRATCH_DIR}/templates")
file(RENAME "${templates_dir}/tree.html" "${templates_dir}/repo-tree.html")
file(RENAME "${templates_dir}/commit.html" "${templates_dir}/commit-page.html")
file(APPEND "${templates_config_file}"
     "\n[templates]\ntree = \"repo-tree.html\"\ncommit = \"commit-page.html\"\n"
)
apps_golden_run_build(
  EXECUTABLE "${SURGE_EXECUTABLE}"
  EMULATOR ${SURGE_EMULATOR}
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
  LABEL "dry run with renamed templates"
  ARGS --dry-run
  STDOUT renamed_stdout
)
string(REGEX MATCHALL "keep\t[^\n]+" renamed_keeps "${renamed_stdout}")
foreach(line IN LISTS renamed_keeps)
  if(line MATCHES "\\.(html|atom)$" AND NOT line MATCHES "^keep\tpublic/[^/]+\\.git/")
    message(FATAL_ERROR "the dry run with renamed templates keeps a page: '${line}'")
  endif()
endforeach()
apps_golden_run_build(
  EXECUTABLE "${SURGE_EXECUTABLE}"
  EMULATOR ${SURGE_EMULATOR}
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
  LABEL "fixture site with renamed templates"
  ARGS --workers 2
)
apps_golden_compare_trees(
  EXPECTED_DIR "${SURGE_EXPECTED_DIR}"
  ACTUAL_DIR "${actual_dir}"
  SCRATCH_DIR "${SURGE_SCRATCH_DIR}"
  VERSION "${SURGE_VERSION}"
  VERSIONED "\\.(html|xml|atom|txt|json)$"
  EXCLUDE "_surge/output-cache.tsv"
)
file(RENAME "${templates_dir}/repo-tree.html" "${templates_dir}/tree.html")
file(RENAME "${templates_dir}/commit-page.html" "${templates_dir}/commit.html")
file(WRITE "${templates_config_file}" "${templates_config_text}")
apps_golden_run_build(
  EXECUTABLE "${SURGE_EXECUTABLE}"
  EMULATOR ${SURGE_EMULATOR}
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
  LABEL "fixture site with its templates put back"
  ARGS --workers 2
)

# Each clone export holds a pack git reads, and the refs of its source.
set(ENV{GIT_CONFIG_NOSYSTEM} 1)
set(ENV{HOME} "${SURGE_SCRATCH_DIR}")
foreach(variable IN ITEMS GIT_DIR GIT_WORK_TREE GIT_CONFIG_GLOBAL GIT_CONFIG_PARAMETERS)
  unset(ENV{${variable}})
endforeach()
file(GLOB export_dirs LIST_DIRECTORIES TRUE "${actual_dir}/*.git")
foreach(export_dir IN LISTS export_dirs)
  cmake_path(GET export_dir STEM LAST_ONLY repo_name)
  file(GLOB idx_files "${export_dir}/objects/pack/*.idx")
  foreach(idx_file IN LISTS idx_files)
    execute_process(
      COMMAND "${SURGE_GIT}" verify-pack "${idx_file}"
      RESULT_VARIABLE verify_result
      ERROR_VARIABLE verify_error
      OUTPUT_QUIET
    )
    if(NOT verify_result EQUAL 0)
      message(FATAL_ERROR "git verify-pack failed for '${idx_file}':\n${verify_error}")
    endif()
  endforeach()
  # A strict index of a copy checks every object of the pack as `git fsck --strict` would, with no
  # server and no clone. The hostile fixture holds the names it refuses on purpose.
  file(GLOB pack_files "${export_dir}/objects/pack/*.pack")
  foreach(pack_file IN LISTS pack_files)
    if(repo_name STREQUAL "hostile")
      continue()
    endif()
    set(strict_dir "${SURGE_SCRATCH_DIR}/strict/${repo_name}")
    file(REMOVE_RECURSE "${strict_dir}")
    file(COPY "${pack_file}" DESTINATION "${strict_dir}")
    cmake_path(GET pack_file FILENAME pack_name)
    execute_process(
      COMMAND "${SURGE_GIT}" index-pack --strict "${strict_dir}/${pack_name}"
      WORKING_DIRECTORY "${strict_dir}"
      RESULT_VARIABLE strict_result
      ERROR_VARIABLE strict_error
      OUTPUT_QUIET
    )
    if(NOT strict_result EQUAL 0)
      message(FATAL_ERROR "git index-pack --strict failed for '${pack_file}':\n${strict_error}")
    endif()
  endforeach()
  execute_process(
    COMMAND "${SURGE_GIT}" for-each-ref "--format=%(objectname)%09%(refname)" refs/heads refs/tags
    WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}/repos/${repo_name}"
    OUTPUT_VARIABLE source_refs
    RESULT_VARIABLE refs_result
  )
  file(STRINGS "${export_dir}/info/refs" info_refs_lines)
  set(listed_refs "")
  foreach(line IN LISTS info_refs_lines)
    if(NOT line MATCHES "\\^\\{\\}$")
      string(APPEND listed_refs "${line}\n")
    endif()
  endforeach()
  if(NOT refs_result EQUAL 0 OR NOT listed_refs STREQUAL source_refs)
    message(FATAL_ERROR "'${repo_name}.git/info/refs' does not list the refs of its source")
  endif()
endforeach()

if(DEFINED SURGE_PYTHON)
  execute_process(
    COMMAND "${SURGE_PYTHON}" -I "${CMAKE_CURRENT_LIST_DIR}/../tests/clone_test.py" "${SURGE_GIT}"
            "${actual_dir}" "${SURGE_SCRATCH_DIR}/repos" "${SURGE_SCRATCH_DIR}/clones"
    RESULT_VARIABLE clone_result
    ERROR_VARIABLE clone_error
  )
  if(NOT clone_result EQUAL 0)
    message(FATAL_ERROR "the clone test failed:\n${clone_error}")
  endif()
endif()

# The mise test installs the monorepo's release project through its example, with every mise
# directory in the scratch tree. A site given mise must have the page, so a renamed fixture cannot
# turn the test off unnoticed.
set(mise_project_page "monorepo/releases/alpha/index.html")
if(DEFINED SURGE_PYTHON AND DEFINED SURGE_MISE)
  if(NOT EXISTS "${actual_dir}/${mise_project_page}")
    message(FATAL_ERROR "the mise test has no project page: '${mise_project_page}'")
  endif()
  execute_process(
    COMMAND "${SURGE_PYTHON}" -I "${CMAKE_CURRENT_LIST_DIR}/../tests/mise_test.py" "${SURGE_MISE}"
            "${actual_dir}" "${SURGE_SCRATCH_DIR}/mise" "https://git.example.org"
            "${mise_project_page}" alpha 0.1.0 0.2.0
    RESULT_VARIABLE mise_result
    OUTPUT_VARIABLE mise_output
    ERROR_VARIABLE mise_error
  )
  if(NOT mise_result EQUAL 0)
    message(FATAL_ERROR "the mise test failed:\n${mise_output}${mise_error}")
  endif()
endif()

# Retention. A commit moves showcase's `main`, so the next build writes a new pack and keeps the old
# one, unlisted, for a client that read the old `info/refs`. The build after that removes it.
set(packs_file "${actual_dir}/showcase.git/objects/info/packs")
file(STRINGS "${packs_file}" old_packs REGEX "^P ")
string(REGEX REPLACE "^P (pack-[0-9a-f]+)\\.pack$" "\\1" old_pack "${old_packs}")
set(ENV{GIT_AUTHOR_NAME} "Ada Lovelace")
set(ENV{GIT_AUTHOR_EMAIL} "ada@example.org")
set(ENV{GIT_AUTHOR_DATE} "1767956400 +0100")
set(ENV{GIT_COMMITTER_NAME} "Ada Lovelace")
set(ENV{GIT_COMMITTER_EMAIL} "ada@example.org")
set(ENV{GIT_COMMITTER_DATE} "1767956400 +0100")
execute_process(
  COMMAND "${SURGE_GIT}" commit --quiet --allow-empty -m "Move main"
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}/repos/showcase"
  RESULT_VARIABLE commit_result
  ERROR_VARIABLE commit_error
)
if(NOT commit_result EQUAL 0)
  message(FATAL_ERROR "moving showcase's main failed:\n${commit_error}")
endif()
set(pack_dir "${actual_dir}/showcase.git/objects/pack")

# A dry run after the move writes only what the moved ref decides: the repo's home and refs pages,
# the site index, the moved branch's log pages and feed, the page of its root, which shows its newest
# commit, the new commit's page, the clone export's changed files and new pack, and the output cache.
# The commit changes no tree, so every other tree page, every blob page and raw copy, and every
# other commit page, is kept. The dry run makes no pack, so it names the new one by its refs.
execute_process(
  COMMAND "${SURGE_GIT}" rev-parse HEAD
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}/repos/showcase"
  OUTPUT_VARIABLE moved_oid
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND ${SURGE_EMULATOR} "${SURGE_EXECUTABLE}" build --dry-run
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
  RESULT_VARIABLE dry_run_result
  OUTPUT_VARIABLE dry_run_stdout
  ERROR_VARIABLE dry_run_stderr
)
if(NOT dry_run_result EQUAL 0)
  message(FATAL_ERROR "the dry run after a move failed:\n${dry_run_stdout}${dry_run_stderr}")
endif()
set(moved_writes
    "public/showcase/commits/main/index.html"
    "public/showcase/commits/main.atom"
    "public/showcase/commit/${moved_oid}/index.html"
    "public/index.html"
    "public/showcase/index.html"
    "public/showcase/tree/main/index.html"
    "public/showcase/branches/index.html"
    "public/showcase/tags/index.html"
    "public/showcase.git/info/refs"
    "public/showcase.git/objects/info/packs"
    "public/_surge/output-cache.tsv"
)
string(REGEX MATCHALL "write\t[^\n]+" written_lines "${dry_run_stdout}")
foreach(line IN LISTS written_lines)
  string(REGEX REPLACE "^write\t" "" written "${line}")
  if(NOT written IN_LIST moved_writes
     AND NOT written MATCHES "^public/showcase\\.git/objects/pack/pack-[0-9a-f]+\\.(pack|idx)$"
     AND NOT written MATCHES "^public/showcase/commits/main/(index|page-[0-9]+)\\.html$"
     AND NOT written STREQUAL "public/showcase/commit/${moved_oid}/index.html"
  )
    message(FATAL_ERROR "the dry run after a move would write an output it does not decide: "
                        "'${written}'")
  endif()
endforeach()
foreach(written IN LISTS moved_writes)
  if(NOT "write\t${written}" IN_LIST written_lines)
    message(FATAL_ERROR "the dry run after a move would keep '${written}'")
  endif()
endforeach()
# A build that fails at a page, before it writes the new pack, still records the old one, so the
# build after it keeps that pack one build, as it would after a build that succeeded.
set(tree_template "${SURGE_SCRATCH_DIR}/templates/tree.html")
file(READ "${tree_template}" tree_template_text)
file(WRITE "${tree_template}" "{{{site.title}}}")
execute_process(
  COMMAND ${SURGE_EMULATOR} "${SURGE_EXECUTABLE}" build
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
  RESULT_VARIABLE build_result
  OUTPUT_QUIET ERROR_QUIET
)
file(WRITE "${tree_template}" "${tree_template_text}")
if(NOT build_result EQUAL 1)
  message(FATAL_ERROR "the build with a broken tree template did not fail")
endif()
foreach(build IN ITEMS moved settled)
  execute_process(
    COMMAND ${SURGE_EMULATOR} "${SURGE_EXECUTABLE}" build --workers 2
    WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
  )
  if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "the ${build} build failed:\n${build_stdout}${build_stderr}")
  endif()
  file(STRINGS "${packs_file}" new_packs REGEX "^P ")
  if(new_packs STREQUAL old_packs)
    message(FATAL_ERROR "the ${build} build lists the pack of the old refs")
  endif()
  if(build STREQUAL "moved")
    if(NOT EXISTS "${pack_dir}/${old_pack}.pack" OR NOT EXISTS "${pack_dir}/${old_pack}.idx")
      message(FATAL_ERROR "the build after a move removed the previous pack '${old_pack}'")
    endif()
  elseif(EXISTS "${pack_dir}/${old_pack}.pack" OR EXISTS "${pack_dir}/${old_pack}.idx")
    message(FATAL_ERROR "the second build after a move kept the previous pack '${old_pack}'")
  endif()
endforeach()

# A release directory added for an old tag, and one removed, change only the repo's releases: the
# dry run writes its release outputs, its tag and release feeds and the tags page that links them, removes the removed
# release's outputs, and keeps everything else. The directories are put back afterwards.
set(showcase_releases "${SURGE_SCRATCH_DIR}/releases/showcase")
if(IS_DIRECTORY "${showcase_releases}/v0.1.0")
  file(WRITE "${showcase_releases}/v0.2.0-nested/nested.txt" "A nested release.\n")
  file(RENAME "${showcase_releases}/v0.1.0" "${SURGE_SCRATCH_DIR}/v0.1.0-aside")
  execute_process(
    COMMAND ${SURGE_EMULATOR} "${SURGE_EXECUTABLE}" build --dry-run
    WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
    RESULT_VARIABLE dry_run_result
    OUTPUT_VARIABLE dry_run_stdout
    ERROR_VARIABLE dry_run_stderr
  )
  if(NOT dry_run_result EQUAL 0)
    message(FATAL_ERROR "the dry run after a release change failed:\n"
                        "${dry_run_stdout}${dry_run_stderr}")
  endif()
  string(REGEX MATCHALL "write\t[^\n]+" written_lines "${dry_run_stdout}")
  foreach(line IN LISTS written_lines)
    if(NOT line MATCHES "^write\tpublic/showcase/(releases/|tags/index\\.html$|tags\\.atom$|releases\\.atom$)"
       AND NOT line STREQUAL "write\tpublic/_surge/output-cache.tsv"
    )
      message(FATAL_ERROR "the dry run after a release change would write an output it does not "
                          "decide: '${line}'")
    endif()
  endforeach()
  foreach(expected IN ITEMS "write\tpublic/showcase/tags/index.html"
                            "write\tpublic/showcase/tags.atom" "write\tpublic/showcase/releases.atom"
                            "write\tpublic/showcase/releases/tag/v0.2.0-nested/index.html"
                            "remove\tpublic/showcase/releases/tag/v0.1.0/index.html"
  )
    string(FIND "${dry_run_stdout}" "${expected}\n" found)
    if(found EQUAL -1)
      message(FATAL_ERROR "the dry run after a release change lacks '${expected}'")
    endif()
  endforeach()
  file(REMOVE_RECURSE "${showcase_releases}/v0.2.0-nested")
  file(RENAME "${SURGE_SCRATCH_DIR}/v0.1.0-aside" "${showcase_releases}/v0.1.0")
  execute_process(
    COMMAND ${SURGE_EMULATOR} "${SURGE_EXECUTABLE}" build --workers 2
    WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
  )
  if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "the build after a release change failed:\n${build_stdout}${build_stderr}")
  endif()
endif()

# A new default branch changes the `repo` map that every page of the repo shows, its Commits link,
# so a dry run writes showcase's pages, its commit pages included, and keeps every page of the other
# repos. The config is put back afterwards.
set(default_config_file "${SURGE_SCRATCH_DIR}/surge.toml")
file(READ "${default_config_file}" default_config_text)
set(showcase_name_text "name = \"showcase\"\n")
string(FIND "${default_config_text}" "${showcase_name_text}" showcase_name_found)
if(showcase_name_found EQUAL -1)
  message(FATAL_ERROR "the site's config lacks the showcase repo the default branch step changes")
endif()
string(REPLACE "${showcase_name_text}" "${showcase_name_text}default_branch = \"feature/x\"\n"
               moved_default_text "${default_config_text}"
)
file(WRITE "${default_config_file}" "${moved_default_text}")
execute_process(
  COMMAND ${SURGE_EMULATOR} "${SURGE_EXECUTABLE}" build --dry-run
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
  RESULT_VARIABLE default_result
  OUTPUT_VARIABLE default_stdout
  ERROR_VARIABLE default_stderr
)
if(NOT default_result EQUAL 0)
  message(FATAL_ERROR "the dry run after a new default branch failed:\n"
                      "${default_stdout}${default_stderr}"
  )
endif()
string(REGEX MATCHALL "(write|keep)\t[^\n]+" default_lines "${default_stdout}")
foreach(line IN LISTS default_lines)
  if(line MATCHES "^keep\tpublic/showcase/(commit|tree|blob|commits)/.*\.html$")
    message(FATAL_ERROR "the dry run after a new default branch keeps a showcase page: '${line}'")
  endif()
  if(line MATCHES "^write\tpublic/(monorepo|hostile|empty)/.*\.html$")
    message(FATAL_ERROR "the dry run after a new default branch writes another repo's page: "
                        "'${line}'")
  endif()
endforeach()
string(FIND "${default_stdout}" "write\tpublic/showcase/commit/119edc43de569e78adf5af0a0608e005771ac3ca/index.html\n"
            default_commit_found
)
if(default_commit_found EQUAL -1)
  message(FATAL_ERROR "the dry run after a new default branch keeps showcase's commit pages")
endif()
file(WRITE "${default_config_file}" "${default_config_text}")

# Next, as they change the site's inputs: a forced dry run keeps no output, not even one whose bytes
# are unchanged. A changed template makes every page again, and keeps the raw copies, the static
# files and the clone exports.
function(surge_dry_run stdout_var)
  execute_process(
    COMMAND ${SURGE_EMULATOR} "${SURGE_EXECUTABLE}" build --dry-run ${ARGN}
    WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "the dry run '${ARGN}' failed:\n${stdout}${stderr}")
  endif()
  set(${stdout_var} "${stdout}" PARENT_SCOPE)
endfunction()
surge_dry_run(forced_stdout --force)
string(REGEX MATCHALL "keep\t[^\n]+" forced_keeps "${forced_stdout}")
if(forced_keeps)
  message(FATAL_ERROR "the forced dry run keeps an output: '${forced_keeps}'")
endif()
file(APPEND "${SURGE_SCRATCH_DIR}/templates/partials/head.html" "<!-- changed -->\n")
surge_dry_run(template_stdout)
string(REGEX MATCHALL "keep\t[^\n]+" template_keeps "${template_stdout}")
foreach(line IN LISTS template_keeps)
  if(line MATCHES "\\.(html|atom)$" AND NOT line MATCHES "^keep\tpublic/[^/]+\\.git/")
    message(FATAL_ERROR "the dry run after a template change keeps a page: '${line}'")
  endif()
endforeach()

# Then a blob over `blob_len_max` and under `raw_len_max`, which the site's own limits give none:
# its page says it is too large, and its raw copy still holds its bytes, though nothing else of the
# build reads them. The copy is checked after a build that makes every output, and again after it is
# removed, when the build keeps the blob's page and reads the blob for the copy alone. A `base_url`
# with a path puts that path before every rule of `_headers`.
set(config_file "${SURGE_SCRATCH_DIR}/surge.toml")
file(READ "${config_file}" config_text)
set(limits_text "blob_len_max = 1200\nraw_len_max = 1000\n")
set(base_url_text "base_url = \"https://git.example.org/\"\n")
string(FIND "${config_text}" "${limits_text}" limits_found)
string(FIND "${config_text}" "${base_url_text}" base_url_found)
if(limits_found EQUAL -1 OR base_url_found EQUAL -1)
  message(FATAL_ERROR "the site's config lacks the limits or the 'base_url' the last step changes")
endif()
string(REPLACE "${limits_text}" "blob_len_max = 100\nraw_len_max = 100000\n" config_text
               "${config_text}"
)
string(REPLACE "${base_url_text}" "base_url = \"https://git.example.org/code/\"\n" config_text
               "${config_text}"
)
file(WRITE "${config_file}" "${config_text}")
set(window_raw "${actual_dir}/showcase/raw/main/lib/table.c")
set(window_page "${actual_dir}/showcase/blob/main/lib/table.c/index.html")
execute_process(
  COMMAND "${SURGE_GIT}" cat-file blob main:lib/table.c
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}/repos/showcase"
  OUTPUT_FILE "${SURGE_SCRATCH_DIR}/table.c"
  RESULT_VARIABLE blob_result
)
if(NOT blob_result EQUAL 0)
  message(FATAL_ERROR "reading 'lib/table.c' of showcase's main failed")
endif()
foreach(build IN ITEMS window removed)
  if(build STREQUAL "removed")
    file(REMOVE "${window_raw}")
  endif()
  execute_process(
    COMMAND ${SURGE_EMULATOR} "${SURGE_EXECUTABLE}" build --workers 2
    WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
  )
  if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "the ${build} build failed:\n${build_stdout}${build_stderr}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${SURGE_SCRATCH_DIR}/table.c" "${window_raw}"
    RESULT_VARIABLE compare_result
    OUTPUT_QUIET ERROR_QUIET
  )
  if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "after the ${build} build, the raw copy of a blob over 'blob_len_max' "
                        "does not hold its bytes: '${window_raw}'")
  endif()
endforeach()
file(READ "${window_page}" window_html)
string(FIND "${window_html}" "<p>This file is too large to show.</p>" too_large_found)
if(too_large_found EQUAL -1)
  message(FATAL_ERROR "the page of a blob over 'blob_len_max' does not say it is too large: "
                      "'${window_page}'")
endif()
file(READ "${SURGE_EXPECTED_DIR}/_headers" expected_headers)
string(REGEX REPLACE "(^|\n)/" "\\1/code/" expected_headers "${expected_headers}")
file(READ "${actual_dir}/_headers" actual_headers)
if(NOT actual_headers STREQUAL expected_headers)
  message(FATAL_ERROR "'_headers' of a 'base_url' with a path differs from expected:\n"
                      "${actual_headers}")
endif()

# Last, a commit that changes one blob of showcase's `main`: a dry run writes that file's blob page
# and raw copy, the tree pages of the directories above it, the pages that show Markdown or Org,
# whose links resolve against the branch's root tree, the pages that the new commit moves, as after
# the move above, and the clone export, and keeps every other output.
set(changed_blob "${SURGE_SCRATCH_DIR}/repos/showcase/lib/main.c")
file(APPEND "${changed_blob}" "/* changed */\n")
execute_process(
  COMMAND "${SURGE_GIT}" commit --quiet -am "Change main.c"
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}/repos/showcase"
  RESULT_VARIABLE commit_result
  ERROR_VARIABLE commit_error
)
if(NOT commit_result EQUAL 0)
  message(FATAL_ERROR "changing showcase's 'lib/main.c' failed:\n${commit_error}")
endif()
execute_process(
  COMMAND "${SURGE_GIT}" rev-parse HEAD
  WORKING_DIRECTORY "${SURGE_SCRATCH_DIR}/repos/showcase"
  OUTPUT_VARIABLE changed_oid
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
surge_dry_run(blob_stdout)
set(blob_writes
    "public/showcase.git/info/refs"
    "public/showcase.git/objects/info/packs"
    "public/showcase/index.html"
    "public/showcase/branches/index.html"
    "public/showcase/tags/index.html"
    "public/showcase/tree/main/index.html"
    "public/showcase/tree/main/docs/index.html"
    "public/showcase/tree/main/lib/index.html"
    "public/showcase/blob/main/CONTRIBUTING.md/index.html"
    "public/showcase/blob/main/README.md/index.html"
    "public/showcase/blob/main/notes.org/index.html"
    "public/showcase/blob/main/docs/CODE_OF_CONDUCT.md/index.html"
    "public/showcase/blob/main/docs/manual.md/index.html"
    "public/showcase/blob/main/lib/README.md/index.html"
    "public/showcase/blob/main/lib/main.c/index.html"
    "public/showcase/raw/main/lib/main.c"
    "public/showcase/commits/main/index.html"
    "public/showcase/commits/main.atom"
    "public/showcase/commit/${changed_oid}/index.html"
    "public/index.html"
    "public/_surge/output-cache.tsv"
)
string(REGEX MATCHALL "[^\n]+" blob_lines "${blob_stdout}")
foreach(line IN LISTS blob_lines)
  if(line MATCHES "^keep\t")
    continue()
  endif()
  string(REGEX REPLACE "^write\t" "" written "${line}")
  if(written STREQUAL line
     OR (NOT written IN_LIST blob_writes
         AND NOT written MATCHES "^public/showcase\\.git/objects/pack/pack-[0-9a-f]+\\.(pack|idx)$"
         AND NOT written MATCHES "^public/showcase/commits/main/page-[0-9]+\\.html$")
  )
    message(FATAL_ERROR "the dry run after a blob change would change an output it does not "
                        "decide: '${line}'")
  endif()
endforeach()
foreach(written IN LISTS blob_writes)
  if(NOT "write\t${written}" IN_LIST blob_lines)
    message(FATAL_ERROR "the dry run after a blob change would keep '${written}'")
  endif()
endforeach()
