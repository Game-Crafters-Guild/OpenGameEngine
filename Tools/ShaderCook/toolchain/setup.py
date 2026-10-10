#!/usr/bin/env python3
"""Provision the shader cook's pinned host tools (naga, tint).

Downloads the binaries pinned by manifest.json into toolchain/<host>/, which is
gitignored, and verifies each against its recorded sha256. Idempotent: a file
that already hashes correctly is left alone, so re-running is cheap.

Usage:
  python Tools/ShaderCook/toolchain/setup.py
  python Tools/ShaderCook/toolchain/setup.py --host macos-arm64   # cross-cook

The assets are on a release of a public repository and download anonymously:
no GitHub account, token or CLI is needed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

kToolchainDir = Path(__file__).resolve().parent
kManifestPath = kToolchainDir / "manifest.json"

# The cook decides which directory it looks in; reuse its host naming so the two
# can never disagree about where a fetched tool belongs.
sys.path.insert(0, str(kToolchainDir.parent))
import shadercook  # noqa: E402

kChunkSize = 1 << 20


class SetupError(Exception):
    pass


def ToolFileName(host: str, tool: str) -> str:
    return tool + (".exe" if host.startswith("windows") else "")


def HashFile(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(kChunkSize), b""):
            digest.update(chunk)
    return digest.hexdigest()


def Download(repo: str, tag: str, asset: str, destination: Path) -> None:
    url = (f"https://github.com/{repo}/releases/download/"
           f"{urllib.parse.quote(tag)}/{urllib.parse.quote(asset)}")
    request = urllib.request.Request(url, headers={"User-Agent": "shadercook-toolchain-setup"})
    partial = destination.with_name(destination.name + ".part")
    try:
        with urllib.request.urlopen(request) as response, partial.open("wb") as handle:
            while True:
                chunk = response.read(kChunkSize)
                if not chunk:
                    break
                handle.write(chunk)
    except urllib.error.HTTPError as error:
        partial.unlink(missing_ok=True)
        if error.code == 404:
            raise SetupError(
                f"no public asset at {url} (HTTP 404). Check that the repo, releaseTag and asset "
                f"names in {kManifestPath.name} match a public release at "
                f"https://github.com/{repo}/releases/tag/{tag}.")
        raise SetupError(f"downloading {url} failed with HTTP {error.code}")
    except urllib.error.URLError as error:
        partial.unlink(missing_ok=True)
        raise SetupError(f"downloading {url} failed: {error.reason}")
    os.replace(partial, destination)


def MakeExecutable(path: Path) -> None:
    if os.name == "nt":
        return
    mode = path.stat().st_mode
    path.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def FetchTool(repo: str, tag: str, pin: dict, destination: Path) -> None:
    if destination.exists():
        print(f"  {destination.name}: hash mismatch, re-downloading")
    print(f"  {destination.name}: fetching {pin['asset']} ({pin['size']} bytes)")
    Download(repo, tag, pin["asset"], destination)

    actual = HashFile(destination)
    if actual != pin["sha256"]:
        destination.unlink(missing_ok=True)
        raise SetupError(
            f"{pin['asset']} failed verification: expected sha256 {pin['sha256']}, got {actual}. "
            f"The release asset does not match the pin in {kManifestPath.name}.")
    MakeExecutable(destination)
    print(f"  {destination.name}: verified")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", help="provision for this host instead of the running one "
                                       "(cross-cooking)")
    args = parser.parse_args()

    manifest = json.loads(kManifestPath.read_text())
    host = args.host or shadercook.HostToolchainDir().name

    hosts = manifest["hosts"]
    if host not in hosts:
        raise SetupError(f"unknown host '{host}' - manifest.json pins {sorted(hosts)}")
    pins = hosts[host]
    if "unavailable" in pins:
        raise SetupError(f"no pinned tools for {host}: {pins['unavailable']}")

    host_dir = kToolchainDir / host
    host_dir.mkdir(parents=True, exist_ok=True)

    print(f"shadercook toolchain: {host} -> {host_dir}")
    stale = []
    for tool, pin in sorted(pins.items()):
        destination = host_dir / ToolFileName(host, tool)
        if destination.exists() and HashFile(destination) == pin["sha256"]:
            MakeExecutable(destination)
            print(f"  {destination.name}: up to date")
        else:
            stale.append((pin, destination))

    for pin, destination in stale:
        FetchTool(manifest["repo"], manifest["releaseTag"], pin, destination)

    print(f"shadercook toolchain: {host} ready (release {manifest['releaseTag']})")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SetupError as error:
        print(f"toolchain setup: {error}", file=sys.stderr)
        sys.exit(2)
