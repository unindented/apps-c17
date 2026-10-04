#!/usr/bin/env python3
"""Checks and updates the vendored projects in `vendor/` from their upstream commits.

`vendor/vendor.toml` records the pin of each project and the upstream files that its directory
holds. Each `THIRD_PARTY_NOTICES.md` that lists a project records the same pin in its
`Vendored at` line.

`check` fetches each pinned commit, makes the copy that the manifest describes, and fails when the
directory differs from it, when a release no longer names its commit, or when a notices file
records another pin. CI runs it.

`sync` fetches the commit of each named project's release, or its pinned commit when it has no
release, replaces the directory with the copy of that commit, and records the pin in the manifest
and the notices. To bump a project, change its `release` or its `commit` in the manifest, as a
Renovate pull request does, and run `sync` with the project's name. For a grammar whose commit
changes, `sync` prints the upstream diff of the query files that `libs/markup/queries/` ports.

Usage:
  vendor/vendor.py check
  vendor/vendor.py sync PROJECT...
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join(ROOT, "vendor")
MANIFEST = "vendor/vendor.toml"
PATCHES = "vendor/patches"
NOTICES = ["apps/*/THIRD_PARTY_NOTICES.md", "libs/THIRD_PARTY_NOTICES.md"]

# A project's heading in a notices file and its `Vendored at` line.
NOTICE_PATTERN = (
    r"### (?P<name>\S+) \((?P<url>https://[^)\s]+)\)\n\n"
    r"(?P<pin>Vendored at (?:release `(?P<release>[^`]+)`, )?commit `(?P<commit>[0-9a-f]{40})`)"
)


def fail(message):
    """Prints a message and exits with status 1."""
    sys.exit(f"vendor.py: {message}")


def git(repo, *args):
    """Runs git in `repo` and returns its standard output as bytes."""
    return subprocess.run(["git", "-C", repo, *args], check=True, capture_output=True).stdout


def pattern_regex(pattern):
    """Compiles a manifest pattern: `*` matches within a segment, and `**/` any directories."""
    regex = ""
    for part in re.split(r"(\*\*/|\*)", pattern):
        if part == "**/":
            regex += "(?:[^/]+/)*"
        elif part == "*":
            regex += "[^/]*"
        else:
            regex += re.escape(part)
    return re.compile(regex + r"\Z")


def matches(path, patterns):
    """Reports whether `path` matches any of `patterns`."""
    return any(pattern_regex(pattern).match(path) for pattern in patterns)


def pin_text(spec):
    """Returns the `Vendored at` line that the notices give the pin of `spec`."""
    release = f"release `{spec['release']}`, " if "release" in spec else ""
    return f"Vendored at {release}commit `{spec['commit']}`"


def notice_paths():
    """Returns the notices files, relative to the root."""
    paths = []
    for pattern in NOTICES:
        paths += sorted(glob.glob(pattern, root_dir=ROOT))
    return paths


def resolve_release(url, release):
    """Returns the commit that the tag of `release`, with or without a `v`, names at `url`."""
    tags = [f"refs/tags/{release}", f"refs/tags/v{release}"]
    out = subprocess.run(
        ["git", "ls-remote", url, *tags, *(f"{tag}^{{}}" for tag in tags)],
        check=True, capture_output=True, text=True,
    ).stdout
    refs = {}
    for line in out.splitlines():
        sha, ref = line.split("\t")
        refs[ref] = sha
    for tag in tags:
        commit = refs.get(f"{tag}^{{}}") or refs.get(tag)
        if commit:
            return commit
    fail(f"no tag for release '{release}' at '{url}'")


def fetch(repo, url, commit):
    """Fetches `commit` from `url` into the scratch repository `repo`, without its history."""
    git(repo, "fetch", "--quiet", "--depth", "1", url, commit)


def make_copy(repo, commit, spec, out):
    """Writes into `out` the files of `spec` at `commit`, with its patches applied."""
    paths = git(repo, "ls-tree", "-r", "--name-only", "-z", commit).decode().split("\0")
    strip = spec.get("strip", "")
    for path in paths:
        if not path or not matches(path, spec["files"]) or matches(path, spec.get("exclude", [])):
            continue
        target = os.path.join(out, path.removeprefix(strip))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as file:
            file.write(git(repo, "show", f"{commit}:{path}"))
    for patch in spec.get("patches", []):
        result = subprocess.run(
            ["git", "apply", "-p1", os.path.join(ROOT, PATCHES, patch)],
            cwd=out, capture_output=True, text=True,
        )
        if result.returncode != 0:
            fail(f"patch '{PATCHES}/{patch}' does not apply: {result.stderr.strip()}")


def tree_files(directory, local):
    """Returns the files under `directory`, relative to it, that no pattern of `local` matches."""
    paths = set()
    for parent, _, names in os.walk(directory):
        for name in names:
            path = os.path.relpath(os.path.join(parent, name), directory)
            if not matches(path, local):
                paths.add(path)
    return paths


def report(message):
    """Prints one problem that `check` finds."""
    print(f"vendor.py: {message}", file=sys.stderr)


def check_notices(manifest):
    """Reports each notices line whose pin differs from the manifest, and returns the count."""
    problems = 0
    for path in notice_paths():
        with open(os.path.join(ROOT, path), encoding="utf-8") as file:
            notices = file.read()
        for match in re.finditer(NOTICE_PATTERN, notices):
            name = match["name"]
            if name not in manifest:
                report(f"'{path}' lists '{name}', which '{MANIFEST}' does not pin")
                problems += 1
                continue
            spec = manifest[name]
            if match["url"] != spec["url"] or match["pin"] != pin_text(spec):
                report(f"'{path}' records another pin for '{name}'; run "
                       f"'vendor/vendor.py sync {name}'")
                problems += 1
    return problems


def check_project(name, spec, repo):
    """Reports each way the directory of `name` differs from its pin, and returns the count."""
    problems = 0
    if "release" in spec:
        tagged = resolve_release(spec["url"], spec["release"])
        if tagged != spec["commit"]:
            report(f"'{name}' release '{spec['release']}' names commit '{tagged}', not the pinned "
                   f"'{spec['commit']}'; run 'vendor/vendor.py sync {name}'")
            problems += 1
    fetch(repo, spec["url"], spec["commit"])
    directory = os.path.join(VENDOR, name)
    local = spec.get("local", [])
    with tempfile.TemporaryDirectory() as out:
        make_copy(repo, spec["commit"], spec, out)
        expected = tree_files(out, [])
        present = tree_files(directory, local)
        for path in sorted(expected - present):
            report(f"'vendor/{name}/{path}' is missing")
            problems += 1
        for path in sorted(present - expected):
            report(f"'vendor/{name}/{path}' is not in the upstream files or 'local'")
            problems += 1
        for path in sorted(expected & present):
            with open(os.path.join(out, path), "rb") as copy, \
                    open(os.path.join(directory, path), "rb") as vendored:
                if copy.read() != vendored.read():
                    report(f"'vendor/{name}/{path}' differs from the copy of commit "
                           f"'{spec['commit']}'")
                    problems += 1
    return problems


def check(manifest, repo):
    """Reports each way `vendor/` and the notices differ from the manifest, and counts the ways."""
    problems = 0
    directories = {entry.name for entry in os.scandir(VENDOR) if entry.is_dir()} - {"patches"}
    for name in sorted(directories - manifest.keys()):
        report(f"'vendor/{name}' has no table in '{MANIFEST}'")
        problems += 1
    for name in sorted(manifest.keys() - directories):
        report(f"'{MANIFEST}' pins '{name}', which 'vendor/' does not hold")
        problems += 1
    for name in sorted(manifest.keys() & directories):
        problems += check_project(name, manifest[name], repo)
    return problems + check_notices(manifest)


def record_commit(manifest_text, name, commit):
    """Returns the manifest text with the `commit` of the table `name` set to `commit`."""
    start = manifest_text.index(f"\n[{name}]\n")
    end = manifest_text.find("\n[", start + 1)
    end = len(manifest_text) if end < 0 else end
    table = re.sub(r'(?m)^commit = "[0-9a-f]{40}"$', f'commit = "{commit}"',
                   manifest_text[start:end], count=1)
    return manifest_text[:start] + table + manifest_text[end:]


def record_notices(manifest):
    """Rewrites the `Vendored at` line of every project in every notices file."""
    for path in notice_paths():
        full = os.path.join(ROOT, path)
        with open(full, encoding="utf-8") as file:
            notices = file.read()

        def repin(match):
            name = match["name"]
            if name not in manifest:
                return match[0]
            return match[0][:match.start("pin") - match.start()] + pin_text(manifest[name])

        updated = re.sub(NOTICE_PATTERN, repin, notices)
        if updated != notices:
            with open(full, "w", encoding="utf-8") as file:
                file.write(updated)


def sync_project(name, spec, repo):
    """Replaces the directory of `name` with the copy of its pin, and returns the new commit."""
    old = spec["commit"]
    new = resolve_release(spec["url"], spec["release"]) if "release" in spec else old
    fetch(repo, spec["url"], new)
    directory = os.path.join(VENDOR, name)
    local = spec.get("local", [])
    for path in tree_files(directory, local):
        os.remove(os.path.join(directory, path))
    make_copy(repo, new, {**spec, "commit": new}, directory)
    for parent, children, files in os.walk(directory, topdown=False):
        if not children and not files and parent != directory:
            os.rmdir(parent)
    if new == old:
        print(f"{name}: commit '{new}'")
        return new
    print(f"{name}: commit '{new}', was '{old}'")
    queries = spec.get("queries", [])
    if queries:
        fetch(repo, spec["url"], old)
        diff = git(repo, "diff", old, new, "--", *queries).decode()
        print(diff if diff else f"{name}: the query files did not change")
    return new


def sync(manifest, manifest_text, repo, names):
    """Syncs each project in `names`, and records the pins in the manifest and the notices."""
    for name in names:
        if name not in manifest:
            fail(f"no project '{name}' in '{MANIFEST}'")
    changed = False
    for name in names:
        commit = sync_project(name, manifest[name], repo)
        if commit != manifest[name]["commit"]:
            manifest[name]["commit"] = commit
            manifest_text = record_commit(manifest_text, name, commit)
            changed = True
    with open(os.path.join(ROOT, MANIFEST), "w", encoding="utf-8") as file:
        file.write(manifest_text)
    record_notices(manifest)
    if changed:
        print("Port any query changes above to libs/markup/queries/, and bump the version of a "
              "cache whose output depends on the project, such as sosig's PAGE_RECIPE_VERSION.")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check", help="verify vendor/ and the notices against the manifest")
    sync_parser = commands.add_parser("sync", help="vendor the pinned commits of projects")
    sync_parser.add_argument("projects", nargs="+", help="projects to sync")
    args = parser.parse_args()

    with open(os.path.join(ROOT, MANIFEST), encoding="utf-8") as file:
        manifest_text = file.read()
    manifest = tomllib.loads(manifest_text)

    with tempfile.TemporaryDirectory() as repo:
        git(repo, "init", "--quiet")
        if args.command == "check":
            problems = check(manifest, repo)
            if problems:
                fail(f"{problems} problem(s) in the vendored sources")
            print(f"vendor.py: {len(manifest)} projects match their pins")
        else:
            sync(manifest, manifest_text, repo, args.projects)


if __name__ == "__main__":
    main()
