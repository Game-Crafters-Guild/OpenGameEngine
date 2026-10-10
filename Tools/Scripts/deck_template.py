#!/usr/bin/env python3
"""Identity checks for cached Linux x64 Player runtimes, shared by Docker and editor.

A runtime is sealed with the fingerprint of the engine sources it was compiled
from. The editor build stamps the fingerprint of the sources the editor was
compiled from next to the editor (`stamp`), and the editor accepts a cached
runtime only when the two match (`verify --identity`), so the check needs no
engine checkout at export time."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

from publish_game import ReleaseError, binary_target, digest, read_json, write_json


INPUTS = ["Engine", "Apps/Player", "Managed", "cmake", "Tools/ShaderReflectHost",
          "Tools/Scripts/build-steamdeck-docker.sh", "Tools/Scripts/deck_template.py",
          "Tools/Scripts/publish_game.py", "Tools/Scripts/steamdeck-launch.sh", ".gitmodules",
          "Tools/Docker/Dockerfile.steamdeck-dev", "CMakeLists.txt", "CMakePresets.json", "vcpkg.json", "vcpkg-configuration.json"]


def fingerprint(repo):
    # Include uncommitted and new source files as well as HEAD. Identical source
    # trees have the same identity on every host, regardless of checkout path.
    result = subprocess.run(["git", "-C", str(repo), "ls-files", "-z", "--cached", "--others",
                             "--exclude-standard", "--", *INPUTS], capture_output=True, check=True)
    checksum = hashlib.sha256()
    for name in sorted(set(result.stdout.split(b"\0")) - {b""}):
        path = repo / name.decode("utf-8")
        if path.is_file():
            checksum.update(name + b"\0")
            # Git normalizes text line endings across Windows/POSIX checkouts.
            # Hash working-tree bytes; a differing checkout conservatively
            # rebuilds instead of accidentally accepting a stale runtime.
            checksum.update(bytes.fromhex(digest(path)))
    return checksum.hexdigest()


def seal(repo, package, identity):
    if identity != fingerprint(repo):
        raise ReleaseError("Engine inputs changed during compilation; rebuild before caching this runtime")
    if binary_target(package / "Player") != ("linux", "x64"):
        raise ReleaseError("Deck template Player must be an x64 ELF executable")
    files = {}
    for path in sorted(package.iterdir()):
        if path.is_file() and (path.name == "Player" or ".so" in path.name):
            if binary_target(path) != ("linux", "x64"):
                raise ReleaseError(f"Wrong architecture in the Deck runtime: {path.name}")
            files[path.name] = digest(path)
    if "libEngine.so" not in files:
        raise ReleaseError("Deck template has no libEngine.so")
    write_json(package / "runtime-template.json", {
        "schemaVersion": 1, "target": "linux-x64", "configuration": "RelWithDebInfo",
        "sourceFingerprint": identity, "files": files,
    })


def stamp(repo, output):
    """Write the source fingerprint for the editor build, rewriting only on change.

    Without git or a checkout the stamp is empty, and the editor then rebuilds
    the runtime instead of accepting one it cannot match."""
    try:
        identity = fingerprint(repo)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Deck template: no engine source identity ({error}); cached runtimes will be rebuilt",
              file=sys.stderr)
        identity = ""
    if not output.is_file() or output.read_text(encoding="utf-8") != identity:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(identity, encoding="utf-8")


def verify(identity, package):
    if not identity:
        raise ReleaseError("This editor build has no engine source identity; rebuild the template")
    manifest = read_json(package / "runtime-template.json")
    if manifest.get("schemaVersion") != 1 or manifest.get("target") != "linux-x64":
        raise ReleaseError("Unsupported Deck template manifest")
    if manifest.get("sourceFingerprint") != identity:
        raise ReleaseError("Deck runtime is stale for this editor's engine sources; rebuild the template")
    if binary_target(package / "Player") != ("linux", "x64"):
        raise ReleaseError("Cached Player has the wrong target architecture")
    files = manifest.get("files", {})
    if not {"Player", "libEngine.so"}.issubset(files):
        raise ReleaseError("Incomplete runtime identity")
    for name, expected in files.items():
        if Path(name).name != name or digest(package / name) != expected:
            raise ReleaseError(f"Deck runtime changed after compilation: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["fingerprint", "stamp", "seal", "verify"])
    parser.add_argument("--repo", type=Path, help="engine checkout (fingerprint, stamp, seal)")
    parser.add_argument("--package", type=Path)
    parser.add_argument("--identity", help="expected source fingerprint (seal, verify)")
    parser.add_argument("--output", type=Path, help="stamp file (stamp)")
    args = parser.parse_args()
    if args.action != "verify" and args.repo is None:
        parser.error(f"{args.action} requires --repo")
    if args.action == "stamp" and args.output is None:
        parser.error("stamp requires --output")
    try:
        if args.action == "fingerprint":
            print(fingerprint(args.repo))
        elif args.action == "stamp":
            stamp(args.repo, args.output)
        elif args.action == "seal":
            seal(args.repo, args.package, args.identity)
        else:
            verify(args.identity, args.package)
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Deck template: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
