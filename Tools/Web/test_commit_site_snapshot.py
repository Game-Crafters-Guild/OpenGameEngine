#!/usr/bin/env python3
"""commit_site_snapshot.sh commits every file of a site byte for byte, even under a git
configuration that converts line endings (core.autocrlf=true, the Git for Windows default),
which once committed the engine's glue files 2 bytes short, or that runs a clean filter (a
global core.attributesFile, as Git LFS installs); and it refuses a commit that still differs
from the files on disk.

    python Tools/Web/test_commit_site_snapshot.py
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

kScript = Path(__file__).resolve().parent / "commit_site_snapshot.sh"


def GitBash() -> str:
    """Git's own bash on Windows, where a bash earlier on PATH may be WSL's
    (C:\\Windows\\System32\\bash.exe), which runs the script in another system; bash elsewhere."""
    if os.name != "nt":
        return shutil.which("bash") or "bash"
    execPath = subprocess.run(["git", "--exec-path"], capture_output=True, text=True, check=True).stdout.strip()
    bash = Path(execPath).parents[2] / "bin" / "bash.exe"   # <Git>/mingw64/libexec/git-core
    if not bash.is_file():
        raise FileNotFoundError(f"{bash} is missing: the test runs the script with Git for Windows' bash")
    return str(bash)


class CommitSiteSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        # A global git configuration that converts line endings, whatever this machine's is, with
        # a clean filter that upper-cases a file and an attributes file a test can point at it.
        self.globalAttributes = root / "gitattributes"
        self.globalAttributes.write_text("")
        self.globalConfig = root / "gitconfig"
        self.globalConfig.write_text("[core]\n\tautocrlf = true\n"
                                     f"\tattributesFile = {self.globalAttributes.as_posix()}\n"
                                     "[filter \"upper\"]\n\tclean = tr a-z A-Z\n")
        self.env = {**os.environ, "GIT_CONFIG_GLOBAL": str(self.globalConfig), "GIT_CONFIG_NOSYSTEM": "1"}
        self.site = root / "site"
        (self.site / "package").mkdir(parents=True)
        (self.site / "index.html").write_bytes(b"<!doctype html>\n")
        (self.site / "package" / "glue.js").write_bytes(b"var a = 1;\r\nvar b = 2;\r\n")
        (self.site / "package" / "pack.part0").write_bytes(bytes(range(256)) * 4)

    def Git(self, *args: str) -> bytes:
        return subprocess.run(["git", "-C", str(self.site), *args], capture_output=True, check=True,
                              env=self.env).stdout

    def Commit(self) -> subprocess.CompletedProcess:
        return subprocess.run([GitBash(), str(kScript), str(self.site), "Site Test",
                               "site@example.com"], capture_output=True, text=True, env=self.env)

    def assertCommittedAsOnDisk(self):
        for path in self.Git("ls-files").decode().split():
            self.assertEqual(self.Git("cat-file", "blob", f"HEAD:{path}"), (self.site / path).read_bytes(), path)

    def test_every_file_is_committed_byte_for_byte(self):
        self.assertEqual(self.Commit().returncode, 0)
        self.assertCommittedAsOnDisk()

    def test_a_global_clean_filter_does_not_rewrite_the_files(self):
        self.globalAttributes.write_text("*.js filter=upper\n")
        self.assertEqual(self.Commit().returncode, 0)
        self.assertCommittedAsOnDisk()

    def test_a_commit_that_differs_from_the_disk_is_refused(self):
        # The repository's own info/attributes outrank the .gitattributes the script writes.
        self.assertEqual(self.Commit().returncode, 0)
        (self.site / ".git" / "info" / "attributes").write_text("*.js filter=upper\n")
        (self.site / "package" / "glue.js").write_bytes(b"var a = 3;\r\n")
        result = self.Commit()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("package/glue.js", result.stderr)


if __name__ == "__main__":
    unittest.main()
