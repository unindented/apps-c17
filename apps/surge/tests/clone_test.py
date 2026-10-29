#!/usr/bin/env python3
"""Clones every clone export of a built site over dumb HTTP, and compares the refs with the source.

The output is served twice on localhost. The first server answers a missing path with 404, as a
static host with a `404.html` does, and every `<r>.git` must clone, with the branches and tags of
its source repository. The second answers a missing path with 200 and an HTML page, as Cloudflare
Pages does for a project without a top-level `404.html`. git asks for paths that do not exist,
first each object as a loose one, and reads the page as their bytes, so a clone of a repository
with objects must report the page as a corrupt object. git 2.56 then finds the objects in the pack
and finishes the clone, so the errors, not the exit status, show what a missing `404.html` does.

Usage: clone_test.py GIT PUBLIC_DIR REPOS_DIR SCRATCH_DIR
"""

import functools
import http.server
import os
import shutil
import subprocess
import sys
import threading


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """Serves files like `python3 -m http.server`, without a line per request."""

    def log_message(self, format, *args):
        pass


class SinglePageHandler(QuietHandler):
    """Answers every missing path with 200 and an HTML page, as a single-page-app host does."""

    def send_error(self, code, message=None, explain=None):
        if code != 404:
            super().send_error(code, message, explain)
            return
        body = b"<!doctype html><html><body>app</body></html>\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def serve(handler, directory):
    """Starts a server for `directory` on a free localhost port, and returns it."""
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), functools.partial(handler, directory=directory))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def git(git_path, env, *args, cwd=None):
    """Runs git and returns the completed process."""
    return subprocess.run([git_path, *args], cwd=cwd, env=env, capture_output=True, text=True)


def refs(git_path, env, repo_dir, prefix, rename=None):
    """Returns `(oid, name)` lines of the refs below `prefix`, renamed below `rename`."""
    out = git(git_path, env, "for-each-ref", "--format=%(objectname) %(refname)", prefix,
              cwd=repo_dir).stdout
    lines = []
    for line in out.splitlines():
        oid, name = line.split(" ", 1)
        if rename is not None:
            if name.endswith("/HEAD"):
                continue
            name = rename + name[len(prefix):]
        lines.append(f"{oid} {name}")
    return sorted(lines)


def main():
    git_path, public_dir, repos_dir, scratch_dir = sys.argv[1:5]
    shutil.rmtree(scratch_dir, ignore_errors=True)
    os.makedirs(scratch_dir)
    env = {"PATH": os.environ.get("PATH", ""), "HOME": scratch_dir, "GIT_CONFIG_NOSYSTEM": "1",
           "GIT_TERMINAL_PROMPT": "0", "LC_ALL": "C"}
    plain = serve(QuietHandler, public_dir)
    single_page = serve(SinglePageHandler, public_dir)
    failures = []
    exports = sorted(name for name in os.listdir(public_dir) if name.endswith(".git"))
    if not exports:
        failures.append("the site has no clone export")
    for export in exports:
        name = export[:-len(".git")]
        source = os.path.join(repos_dir, name)
        clone = os.path.join(scratch_dir, "plain", name)
        url = f"http://127.0.0.1:{plain.server_address[1]}/{export}"
        result = git(git_path, env, "clone", "--quiet", "--no-checkout", url, clone)
        if result.returncode != 0:
            failures.append(f"cloning '{export}' failed: {result.stderr.strip()}")
            continue
        branches = refs(git_path, env, source, "refs/heads/", "refs/remotes/origin/")
        cloned_branches = refs(git_path, env, clone, "refs/remotes/origin/", "refs/remotes/origin/")
        tags = refs(git_path, env, source, "refs/tags/")
        cloned_tags = refs(git_path, env, clone, "refs/tags/")
        if branches != cloned_branches or tags != cloned_tags:
            failures.append(f"the clone of '{export}' holds other refs than its source")
        if not branches:
            continue
        # The hostile fixture holds the names fsck refuses on purpose, so it is the one left out.
        fsck = git(git_path, env, "fsck", "--strict", "--no-dangling", cwd=clone)
        if fsck.returncode != 0 and name != "hostile":
            failures.append(f"the clone of '{export}' fails git fsck: {fsck.stderr.strip()}")
        single_page_clone = os.path.join(scratch_dir, "single-page", name)
        single_page_url = f"http://127.0.0.1:{single_page.server_address[1]}/{export}"
        result = git(git_path, env, "clone", "--quiet", "--no-checkout", single_page_url,
                     single_page_clone)
        if "inflate" not in result.stderr and result.returncode == 0:
            failures.append(f"cloning '{export}' from a host that answers 200 for a missing path "
                            "read no page as an object")
    plain.shutdown()
    single_page.shutdown()
    for failure in failures:
        print(f"clone_test.py: {failure}", file=sys.stderr)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
