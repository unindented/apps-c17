#include <acutest.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "testing/test_support.h"
#include "vcs/refset.h"
#include "vcs/repository.h"
#include "vcs/test_vcs_support.h"

// The branches and tags load sorted by name, each tag peeled to the object it names: a commit, a
// tag of a tag, and a tag of a tree, which peels to no commit.
static void test_load_peels_tags(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  TEST_ASSERT(open_fixture(&repository, "showcase", &arena) == 0);
  struct Refset refset = {0};
  struct Log log = test_log();
  TEST_CHECK(refset_load(&refset, &repository, &arena, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_ASSERT(refset.count == 6);
  TEST_CHECK(str_eq(refset.refs[0].name, S("refs/heads/feature/x")));
  TEST_CHECK(str_eq(refset.refs[0].short_name, S("feature/x")) &&
             refset.refs[0].kind == REF_KIND_BRANCH);
  const struct Ref* tree_tag = &refset.refs[2];
  TEST_CHECK(str_eq(tree_tag->name, S("refs/tags/tree-tag")) && tree_tag->kind == REF_KIND_TAG);
  TEST_CHECK(tree_tag->peeled_oid.len == 40 && tree_tag->commit_oid.len == 0);
  const struct Ref* nested = &refset.refs[5];
  TEST_CHECK(str_eq(nested->name, S("refs/tags/v0.2.0-nested")));
  TEST_CHECK(str_eq(nested->peeled_oid, refset.refs[4].peeled_oid));
  TEST_CHECK(str_eq(nested->commit_oid, S("f39d4f8eed9b346a0f6e4b51fa0ae919559bc871")));
  const struct Ref* lightweight = &refset.refs[3];
  TEST_CHECK(lightweight->peeled_oid.len == 0 && str_eq(lightweight->commit_oid, lightweight->oid));
  TEST_CHECK(refset_find_branch(&refset, S("main")) == &refset.refs[1]);
  TEST_CHECK(refset_find_branch(&refset, S("v0.1.0")) == NULL);
  repository_close(&repository);
}

// `info/refs` lists each ref and, after an annotated tag, its peeled id, as git writes it.
static void test_format_info_refs_follows_annotated_tags_with_peeled_ids(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  TEST_ASSERT(open_fixture(&repository, "monorepo", &arena) == 0);
  struct Refset refset = {0};
  struct Log log = test_log();
  TEST_ASSERT(refset_load(&refset, &repository, &arena, &log) == 0);
  TEST_CHECK(test_text_is(refset_format_info_refs(&refset, &arena),
                          "c5fd8b3293be898b7ec6ccd749ae30d0d6fa4071\trefs/heads/main\n"
                          "f1a6c44e8dcb3f29a9a4b141ac84e2389f61b8c9\trefs/tags/alpha-v0.1.0\n"
                          "77f1541bfa4365b6350afb8d304d09a01d80bc8e\trefs/tags/alpha-v0.1.0^{}\n"
                          "eb78090e19b5347e704ccec0a696aa5149fa3e82\trefs/tags/alpha-v0.10.0-rc.1\n"
                          "d3312a9c1a7d36206117b1c6644c499873ac091d\trefs/tags/alpha-v0.2.0\n"
                          "eb78090e19b5347e704ccec0a696aa5149fa3e82\trefs/tags/alpha-v0.2.0^{}\n"
                          "eb78090e19b5347e704ccec0a696aa5149fa3e82\trefs/tags/alpha-vnext\n"));
  repository_close(&repository);
}

// A repository with no commit has no refs, and an empty `info/refs`.
static void test_load_empty_repository(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  TEST_ASSERT(open_fixture(&repository, "empty", &arena) == 0);
  struct Refset refset = {0};
  struct Log log = test_log();
  TEST_CHECK(refset_load(&refset, &repository, &arena, &log) == 0);
  TEST_CHECK(refset.count == 0);
  TEST_CHECK(refset_format_info_refs(&refset, &arena).len == 0);
  repository_close(&repository);
}

// Branches whose names, or the directories above them, differ only in case refuse the load, naming
// both, and so does a ref whose name is not UTF-8, each reported in one run.
static void test_load_rejects_branches_colliding_in_case(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  TEST_ASSERT(open_fixture(&repository, "branch-case", &arena) == 0);
  struct Refset refset = {0};
  struct Log log = test_log();
  TEST_CHECK(refset_load(&refset, &repository, &arena, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "ref name is not valid UTF-8: 'refs/heads/caf\\xE9' (in repo "
                         "'branch-case')\n"
                         "branches 'refs/heads/A/b' and 'refs/heads/a' collide after case "
                         "folding (in repo 'branch-case')\n"
                         "branches 'refs/heads/Main' and 'refs/heads/main' collide after case "
                         "folding (in repo 'branch-case')\n"
                         "branches 'refs/heads/X/x' and 'refs/heads/x/y' collide after case "
                         "folding (in repo 'branch-case')\n"));
  repository_close(&repository);
}

// A ref that is a `/`-prefix of another refuses the load, naming both.
static void test_load_rejects_a_ref_prefix(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  TEST_ASSERT(open_fixture(&repository, "ref-prefix", &arena) == 0);
  struct Refset refset = {0};
  struct Log log = test_log();
  TEST_CHECK(refset_load(&refset, &repository, &arena, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "ref 'refs/heads/feature' is a prefix of ref 'refs/heads/feature/x' "
                         "(in repo 'ref-prefix')\n"));
  repository_close(&repository);
}

TEST_LIST = {
    {"load peels tags", test_load_peels_tags},
    {"format info refs follows annotated tags with peeled ids",
     test_format_info_refs_follows_annotated_tags_with_peeled_ids},
    {"load empty repository", test_load_empty_repository},
    {"load rejects branches colliding in case", test_load_rejects_branches_colliding_in_case},
    {"load rejects a ref prefix", test_load_rejects_a_ref_prefix},
    {NULL, NULL},
};
