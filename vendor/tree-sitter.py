#!/usr/bin/env python3
"""Checks and updates the vendored tree-sitter runtime and grammars from their upstream commits.

`vendor/tree-sitter.toml` lists the upstream files of each project that `vendor/<project>/` holds.
`apps/sosig/THIRD_PARTY_NOTICES.md` records the release and the commit of each project, in the line
`Vendored at release `<tag>`, commit `<sha>`.` that Renovate also reads.

`check` fetches each recorded commit and fails when its tag no longer names that commit, or when
`vendor/<project>/` does not hold exactly the listed files of that commit. CI runs it.

`sync` fetches the commit that each recorded tag names now, replaces `vendor/<project>/` with the
listed files of that commit, and records the commit in the notices. To bump a project, change its
release in the notices, as a Renovate pull request does, and run `sync` with the project's name. For
a project whose commit changes, `sync` prints the upstream diff of the query files that sosig keeps
in `apps/sosig/src/formats/queries/`, to port by hand.

Usage:
  vendor/tree-sitter.py check
  vendor/tree-sitter.py sync [PROJECT...]
"""

import argparse
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = "vendor/tree-sitter.toml"
NOTICES = "apps/sosig/THIRD_PARTY_NOTICES.md"

# The heading and the release line of one project in the notices, as Renovate's regex reads them.
NOTICE_PATTERN = (
    r"### {name} \((?P<url>https://[^)\s]+)\)\n\n"
    r"Vendored at release `(?P<tag>[^`]+)`, commit `(?P<commit>[0-9a-f]{{40}})`"
)


def git(repo, *args):
    """Runs git in `repo` and returns its standard output as bytes."""
    return subprocess.run(["git", "-C", repo, *args], check=True, capture_output=True).stdout


def read_notice(notices, name):
    """Returns the match of project `name`'s heading and release line in the text `notices`."""
    match = re.search(NOTICE_PATTERN.format(name=re.escape(name)), notices)
    if match is None:
        sys.exit(f"tree-sitter.py: no release line for '{name}' in '{NOTICES}'")
    return match


def resolve_tag(url, tag):
    """Returns the commit that `tag` names at `url`, peeling an annotated tag."""
    out = subprocess.run(
        ["git", "ls-remote", url, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"],
        check=True, capture_output=True, text=True,
    ).stdout
    refs = {}
    for line in out.splitlines():
        sha, ref = line.split("\t")
        refs[ref] = sha
    commit = refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")
    if commit is None:
        sys.exit(f"tree-sitter.py: no tag '{tag}' at '{url}'")
    return commit


def fetch(repo, url, commit):
    """Fetches `commit` from `url` into the scratch repository `repo`, without its history."""
    git(repo, "fetch", "--quiet", "--depth", "1", url, commit)


def upstream_files(repo, commit, spec):
    """Maps each vendored path of `spec` at `commit` to its upstream path."""
    paths = git(repo, "ls-tree", "-r", "--name-only", "-z", commit).decode().split("\0")
    strip = spec.get("strip", "")
    files = {}
    for path in paths:
        if not any(fnmatch.fnmatchcase(path, pattern) for pattern in spec["files"]):
            continue
        if any(fnmatch.fnmatchcase(path, pattern) for pattern in spec.get("exclude", [])):
            continue
        files[path.removeprefix(strip)] = path
    return files


def vendored_files(directory):
    """Returns the paths of the files under `directory`, relative to it."""
    paths = set()
    for parent, _, names in os.walk(directory):
        for name in names:
            paths.add(os.path.relpath(os.path.join(parent, name), directory))
    return paths


def report(message):
    """Prints one problem that `check` finds."""
    print(f"tree-sitter.py: {message}", file=sys.stderr)


def check(manifest, notices, repo):
    """Reports each way the vendored projects differ from their recorded commits, and returns the
    number of problems."""
    problems = 0
    vendor = os.path.join(ROOT, "vendor")
    projects = {name for name in os.listdir(vendor)
                if name.startswith("tree-sitter") and os.path.isdir(os.path.join(vendor, name))}
    for name in sorted(projects - manifest.keys()):
        report(f"'vendor/{name}' has no entry in '{MANIFEST}'")
        problems += 1
    for name, spec in manifest.items():
        notice = read_notice(notices, name)
        url, tag, commit = notice["url"], notice["tag"], notice["commit"]
        tagged = resolve_tag(url, tag)
        if tagged != commit:
            report(f"'{name}' release '{tag}' names commit '{tagged}', not the recorded "
                   f"'{commit}'; run 'vendor/tree-sitter.py sync {name}'")
            problems += 1
        fetch(repo, url, commit)
        expected = upstream_files(repo, commit, spec)
        directory = os.path.join(ROOT, "vendor", name)
        present = vendored_files(directory)
        for path in sorted(expected.keys() - present):
            report(f"'vendor/{name}/{path}' is missing")
            problems += 1
        for path in sorted(present - expected.keys()):
            report(f"'vendor/{name}/{path}' is not in the upstream files")
            problems += 1
        for path in sorted(expected.keys() & present):
            with open(os.path.join(directory, path), "rb") as file:
                if file.read() != git(repo, "show", f"{commit}:{expected[path]}"):
                    report(f"'vendor/{name}/{path}' differs from commit '{commit}'")
                    problems += 1
    return problems


def sync(manifest, notices, repo, names):
    """Replaces each named project with its tag's commit, and returns the updated notices."""
    for name in names:
        if name not in manifest:
            sys.exit(f"tree-sitter.py: no project '{name}' in '{MANIFEST}'")
    changed = False
    for name in names:
        spec = manifest[name]
        notice = read_notice(notices, name)
        url, tag, old = notice["url"], notice["tag"], notice["commit"]
        new = resolve_tag(url, tag)
        fetch(repo, url, new)
        directory = os.path.join(ROOT, "vendor", name)
        shutil.rmtree(directory)
        for path, upstream in upstream_files(repo, new, spec).items():
            target = os.path.join(directory, path)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as file:
                file.write(git(repo, "show", f"{new}:{upstream}"))
        start, end = notice.span("commit")
        notices = notices[:start] + new + notices[end:]
        if new == old:
            print(f"{name}: '{tag}' is still commit '{new}'")
            continue
        print(f"{name}: '{tag}' is commit '{new}', was '{old}'")
        changed = True
        queries = spec.get("queries", [])
        if queries:
            fetch(repo, url, old)
            diff = git(repo, "diff", old, new, "--", *queries).decode()
            print(diff if diff else f"{name}: the query files did not change")
    if changed:
        print("Port any query changes above to apps/sosig/src/formats/queries/, and bump "
              "PAGE_RECIPE_VERSION in apps/sosig/src/domain/build_cache.h.")
    return notices


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check", help="verify vendor/ against the recorded commits")
    sync_parser = commands.add_parser("sync", help="vendor the commits the recorded tags name")
    sync_parser.add_argument("projects", nargs="*", help="projects to sync (default: all)")
    args = parser.parse_args()

    with open(os.path.join(ROOT, MANIFEST), "rb") as file:
        manifest = tomllib.load(file)
    with open(os.path.join(ROOT, NOTICES), encoding="utf-8") as file:
        notices = file.read()

    with tempfile.TemporaryDirectory() as repo:
        git(repo, "init", "--quiet")
        if args.command == "check":
            problems = check(manifest, notices, repo)
            if problems:
                sys.exit(f"tree-sitter.py: {problems} problem(s) in the vendored sources")
            print(f"tree-sitter.py: {len(manifest)} projects match their recorded commits")
        else:
            updated = sync(manifest, notices, repo, args.projects or list(manifest))
            if updated != notices:
                with open(os.path.join(ROOT, NOTICES), "w", encoding="utf-8") as file:
                    file.write(updated)


if __name__ == "__main__":
    main()
