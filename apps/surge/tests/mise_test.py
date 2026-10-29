#!/usr/bin/env python3
"""Installs a release project of a built site with mise, through the example its release page shows.

The output is copied and served on localhost, and the example `mise.toml` of `PROJECT_PAGE` is read
from the page, with the site's `BASE_URL` swapped for the server's. Every mise directory, `HOME`
and the config files mise reads stay in the scratch tree, so the user's own mise is never touched.

`mise ls-remote` must list the project's versions but its pre-releases, which mise passes over.
`mise lock` must record the `SHA256SUMS` checksum of this host's archive, and `mise install` and
`mise exec` must then run the binary of the newest version. Two more runs must fail: one whose
`SHA256SUMS` was tampered with before a fresh lock, and one whose archive was tampered with after a
good lock, because only the lock makes mise check a download.

Usage: mise_test.py MISE PUBLIC_DIR SCRATCH_DIR BASE_URL PROJECT_PAGE TOOL VERSION_LINE...
"""

import functools
import gzip
import html
import http.server
import os
import re
import shutil
import subprocess
import sys
import threading


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """Serves files like `python3 -m http.server`, without a line per request."""

    def log_message(self, format, *args):
        pass


def serve(directory):
    """Starts a server for `directory` on a free localhost port, and returns it."""
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), functools.partial(QuietHandler, directory=directory))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def fail(message):
    """Ends the test with `message`."""
    sys.exit(f"mise test: {message}")


class Mise:
    """Runs mise in a work directory of its own, with every directory it writes in `root`."""

    def __init__(self, mise_path, root, config):
        self.mise_path = mise_path
        self.root = root
        shutil.rmtree(root, ignore_errors=True)
        self.work_dir = os.path.join(root, "work")
        for name in ("data", "cache", "state", "config", "home", "work"):
            os.makedirs(os.path.join(root, name))
        with open(os.path.join(self.work_dir, "mise.toml"), "w", encoding="utf-8") as file:
            file.write(config)
        self.env = {
            "PATH": os.environ.get("PATH", ""),
            "HOME": os.path.join(root, "home"),
            "MISE_DATA_DIR": os.path.join(root, "data"),
            "MISE_CACHE_DIR": os.path.join(root, "cache"),
            "MISE_STATE_DIR": os.path.join(root, "state"),
            "MISE_CONFIG_DIR": os.path.join(root, "config"),
            "MISE_GLOBAL_CONFIG_FILE": os.path.join(root, "config", "config.toml"),
            "MISE_CEILING_PATHS": root,
            "MISE_TRUSTED_CONFIG_PATHS": self.work_dir,
            "MISE_YES": "1",
        }

    def run(self, *args):
        """Runs mise with `args` and returns the completed process."""
        return subprocess.run([self.mise_path, *args], cwd=self.work_dir, env=self.env,
                              capture_output=True, text=True)

    def check(self, *args):
        """Runs mise with `args`, ends the test when it fails, and returns its output."""
        result = self.run(*args)
        if result.returncode != 0:
            fail(f"'mise {' '.join(args)}' failed:\n{result.stdout}{result.stderr}")
        return result.stdout


def example(public_dir, project_page, base_url, url):
    """Returns the example `mise.toml` of a release project's page, pointed at `url`."""
    with open(os.path.join(public_dir, project_page), encoding="utf-8") as file:
        page = file.read()
    match = re.search(r"<pre><code>(.*?)</code></pre>", page, re.S)
    if match is None:
        fail(f"'{project_page}' shows no mise example")
    return html.unescape(match.group(1)).replace(base_url, url)


def tamper(file_path):
    """Replaces an archive with another valid one of the same files, gzipped with another mtime, so
    only its checksum tells the two apart."""
    with gzip.open(file_path, "rb") as file:
        data = file.read()
    with open(file_path, "wb") as file:
        with gzip.GzipFile(filename="", mode="wb", fileobj=file, mtime=1) as archive:
            archive.write(data)


def check_refused(result, what):
    """Ends the test unless `result` failed on a checksum mismatch."""
    output = result.stdout + result.stderr
    if result.returncode == 0 or not re.search(r"checksum mismatch", output, re.I):
        fail(f"{what}: mise did not refuse it on its checksum:\n{output}")


def main():
    mise_path, public_dir, scratch_dir, base_url, project_page, tool = sys.argv[1:7]
    version_lines = sys.argv[7:]
    shutil.rmtree(scratch_dir, ignore_errors=True)
    served_dir = os.path.join(scratch_dir, "public")
    shutil.copytree(public_dir, served_dir)
    server = serve(served_dir)
    url = f"http://127.0.0.1:{server.server_address[1]}"
    config = example(served_dir, project_page, base_url, url)
    downloads = re.findall(r'url = "([^"]*\{\{ version \}\}[^"]*)"', config)
    version = version_lines[-1]

    good = Mise(mise_path, os.path.join(scratch_dir, "good"), config)
    listed = good.check("ls-remote", f"http:{tool}").split()
    if listed != version_lines:
        fail(f"'mise ls-remote' lists {listed}, not {version_lines}")
    good.check("lock")
    with open(os.path.join(good.work_dir, "mise.lock"), encoding="utf-8") as file:
        lock = file.read()
    sums_url = next(u for u in downloads if u.endswith("/SHA256SUMS"))
    sums_path = os.path.join(served_dir, sums_url.replace("{{ version }}", version)[len(url) + 1:])
    with open(sums_path, encoding="utf-8") as file:
        sums = [line.split()[0] for line in file if line.strip()]
    if not any(f"sha256:{digest}" in lock for digest in sums):
        fail("'mise lock' recorded no checksum of 'SHA256SUMS'")
    good.check("install")
    output = good.check("exec", "--", tool).strip()
    if output != f"{tool} {version}":
        fail(f"the installed '{tool}' printed '{output}'")

    # A tampered `SHA256SUMS` goes into a fresh lock, so the download no longer matches it.
    with open(sums_path, encoding="utf-8") as file:
        original_sums = file.read()
    with open(sums_path, "w", encoding="utf-8") as file:
        file.write(re.sub(r"^[0-9a-f]{64}", "0" * 64, original_sums, flags=re.M))
    bad_sums = Mise(mise_path, os.path.join(scratch_dir, "bad-sums"), config)
    bad_sums.run("lock")
    check_refused(bad_sums.run("install"), "an archive its tampered 'SHA256SUMS' does not match")
    with open(sums_path, "w", encoding="utf-8") as file:
        file.write(original_sums)

    # A good lock refuses an archive tampered with after it was written.
    bad_archive = Mise(mise_path, os.path.join(scratch_dir, "bad-archive"), config)
    bad_archive.check("lock")
    archives = [u.replace("{{ version }}", version)[len(url) + 1:] for u in downloads
                if u.endswith(".tar.gz")]
    for archive in archives:
        tamper(os.path.join(served_dir, archive))
    check_refused(bad_archive.run("install"), "a tampered archive behind a good lock")
    server.shutdown()


if __name__ == "__main__":
    main()
