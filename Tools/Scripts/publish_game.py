#!/usr/bin/env python3
"""Portable game release staging, SteamPipe publishing, and SSH Deck deployment.

Python 3.10+, standard library only. Build recipes are argument arrays, never
shell fragments. Steam credentials stay in SteamCMD's own authenticated session.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid


class ReleaseError(ValueError):
    pass


def read_json(path):
    with open(path, encoding="utf-8-sig") as stream:
        return json.load(stream)


def write_json(path, data):
    Path(path).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def relative(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ReleaseError(f"Expected a portable relative path: {value!r}")
    path = PurePosixPath(value)
    if path.is_absolute() or any(p in ("..", ".") for p in value.split("/")):
        raise ReleaseError(f"Path must stay inside its package: {value!r}")
    return Path(*path.parts)


def under(root, value):
    path = root / relative(value)
    if not path.resolve().is_relative_to(root.resolve()):
        raise ReleaseError(f"Path escapes its package: {value}")
    return path


def identifier(value, label):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,79}", str(value)):
        raise ReleaseError(f"{label} must use letters, digits, dots, underscores or hyphens")
    return str(value)


def steam_id(value, label):
    if isinstance(value, bool) or not re.fullmatch(r"[1-9][0-9]*", str(value)):
        raise ReleaseError(f"Set a positive {label} in the publishing configuration")
    return str(value)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def tree_manifest(root):
    entries, folded = {}, {}
    for path in sorted(root.rglob("*")):
        name = path.relative_to(root).as_posix()
        # Windows and commonly macOS use case-insensitive filesystems.
        if name.casefold() in folded:
            raise ReleaseError(f"Case-colliding package paths: {name} and {folded[name.casefold()]}")
        folded[name.casefold()] = name
        if path.is_symlink():
            target = os.readlink(path)
            if os.path.isabs(target) or not path.resolve(strict=True).is_relative_to(root.resolve()):
                raise ReleaseError(f"Non-relocatable symlink: {name}")
            entries[name] = {"symlink": target}
        elif path.is_file():
            entries[name] = {"sha256": digest(path), "size": path.stat().st_size}
        elif not path.is_dir():
            raise ReleaseError(f"Unsupported package entry: {name}")
    return entries


def binary_target(path):
    with path.open("rb") as stream:
        data = stream.read(4096)
        if data[:4] == b"\x7fELF" and len(data) >= 20:
            endian = "<" if data[5] == 1 else ">"
            machine = struct.unpack_from(endian + "H", data, 18)[0]
            return "linux", {62: "x64", 183: "arm64"}.get(machine, "unknown")
        if data[:2] == b"MZ" and len(data) >= 64:
            stream.seek(struct.unpack_from("<I", data, 60)[0])
            pe = stream.read(6)
            if pe[:4] == b"PE\0\0":
                return "windows", {0x8664: "x64", 0xAA64: "arm64"}.get(
                    struct.unpack_from("<H", pe, 4)[0], "unknown")
        if data[:4] in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf") and len(data) >= 8:
            endian = "<" if data[0] == 0xcf else ">"
            cpu = struct.unpack_from(endian + "I", data, 4)[0]
            return "macos", {0x1000007: "x64", 0x100000c: "arm64"}.get(cpu, "unknown")
        if data[:4] in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
            return "macos", "universal"
    raise ReleaseError(f"Not a supported native Player executable: {path}")


def validate_payload(root, target):
    if not root.is_dir():
        raise ReleaseError(f"Build output does not exist: {root}")
    executable = under(root, target["executable"])
    actual = binary_target(executable)
    expected = (target["platform"], target["architecture"])
    if actual != expected:
        raise ReleaseError(f"Wrong Player binary in {root}: expected {expected}, found {actual}")
    config_path = under(root, target["gameConfig"])
    config = read_json(config_path)
    content = config_path.parent
    for field in ("startupScene", "renderPipeline"):
        value = config.get(field)
        if not value or not under(content / "Assets", value).is_file():
            raise ReleaseError(f"Missing {field} asset in {config_path}: {value!r}")
    assembly = config.get("scriptAssemblyPath")
    if assembly:
        # Resolved against the content folder and constrained to the depot root.
        assembly_path = (content / assembly).resolve()
        if not assembly_path.is_relative_to(root.resolve()) or not assembly_path.is_file():
            raise ReleaseError(f"Missing or non-relocatable gameplay assembly: {assembly}")
    aot_name = {"windows": "GameEngine.Scripts.native.dll", "macos": "GameEngine.Scripts.native.dylib",
                "linux": "GameEngine.Scripts.native.so"}[target["platform"]]
    # NativeAOT gameplay is code, so it sits beside the executable; on macOS that is
    # Contents/MacOS, while game.config and Assets/ are in Contents/Resources.
    if list((content / "Assets").rglob("*.cs")) and not assembly and not (executable.parent / aot_name).is_file():
        raise ReleaseError("C# sources are present but no compiled gameplay was staged; rebuild through the native target pipeline")
    if not (content / "Assets/.assetmanifest").is_file():
        raise ReleaseError(f"Missing .assetmanifest in {content}; export through the current build pipeline")
    if not list((content / "Assets").rglob("*.shaderpkg")):
        raise ReleaseError(f"No compiled shader packages in {content / 'Assets'}")
    index = content / "Packages/packages.index"
    if index.is_file():
        packages = read_json(index).get("packages")
        if not isinstance(packages, list):
            raise ReleaseError("packages.index must contain a packages array")
        aliases = set()
        for package in packages:
            alias = identifier(package["alias"], "Package alias")
            if alias in aliases:
                raise ReleaseError(f"Duplicate package alias: {alias}")
            aliases.add(alias)
            package_root = content / "Packages" / alias
            if not (package_root / "Assets/.assetmanifest").is_file():
                raise ReleaseError(f"Incomplete staged package: {alias}")
            for module in package.get("nativeModules", []):
                identifier(module, "Native module")
                record = package_root / "NativeScripts/last_build.txt"
                if not record.is_file():
                    raise ReleaseError(f"Missing native package module record: {alias}/{module}")
                lines = record.read_text(encoding="utf-8").splitlines()
                if len(lines) < 3 or not lines[2] or not (package_root / "NativeScripts/engine_abi.txt").is_file():
                    raise ReleaseError(f"Missing native module ABI identity: {alias}/{module}")
                if binary_target(under(package_root, lines[1])) != expected:
                    raise ReleaseError(f"Wrong native package module architecture: {alias}/{module}")
    for item in target.get("requiredFiles", []):
        if not under(root, item).is_file():
            raise ReleaseError(f"Missing required build output: {item}")
    if target["platform"] == "linux":
        for library in root.rglob("*"):
            if library.is_file() and (library.name.endswith(".so") or ".so." in library.name):
                if binary_target(library) != expected:
                    raise ReleaseError(f"Wrong Linux runtime library architecture: {library}")
    if target.get("deviceProfile") == "steamdeck" and expected != ("linux", "x64"):
        raise ReleaseError("The native Steam Deck profile requires Linux x64")
    if os.name != "nt" and not executable.stat().st_mode & 0o111:
        raise ReleaseError(f"Player is not executable: {executable}")
    # Traverse before copying: reject dangling/outside symlinks and case collisions.
    tree_manifest(root)
    return config


def load_config(path):
    path = path.resolve()
    config = read_json(path)
    if config.get("schemaVersion") != 1 or not isinstance(config.get("targets"), dict):
        raise ReleaseError("Expected publishing schemaVersion 1 and a targets object")
    for name, target in config["targets"].items():
        identifier(name, "Target name")
        if target.get("platform") not in ("windows", "macos", "linux"):
            raise ReleaseError(f"Unsupported platform for {name}")
        if target.get("architecture") not in ("x64", "arm64", "universal"):
            raise ReleaseError(f"Unsupported architecture for {name}")
        for field in ("executable", "gameConfig"):
            relative(target[field])
    return path.parent, config


def selected_targets(config, selection):
    names = selection or list(config["targets"])
    if not names or len(set(names)) != len(names):
        raise ReleaseError("Select at least one target, without duplicates")
    if any(name not in config["targets"] for name in names):
        raise ReleaseError("Unknown target; use a name from the publishing configuration")
    return {name: config["targets"][name] for name in names}


def source_path(base, target):
    return (base / Path(target["source"]).expanduser()).resolve()


def host_platform():
    return "windows" if os.name == "nt" else "macos" if sys.platform == "darwin" else "linux"


def editor_call(method, params=None, port=None):
    repo = Path(__file__).resolve().parents[2]
    command = ["node", str(repo / "mcp/ge.mjs"), "--json"]
    if port:
        command += ["--port", str(port)]
    command += ["raw", method, json.dumps(params or {})]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise ReleaseError(f"Editor request {method} failed: {result.stdout or result.stderr}")
    response = json.loads(result.stdout)
    data = response.get("data", {})
    if not response.get("ok") or data.get("error"):
        raise ReleaseError(f"Editor request {method}: {data.get('error', response)}")
    return data


def build_in_editor(base, target, port=None, timeout=3600):
    settings = editor_call("get_build_settings", port=port)
    if Path(settings["workspaceRoot"]).resolve() != base.resolve():
        raise ReleaseError("The connected editor has a different project open. Open this project or select --editor-port.")
    before = editor_call("get_build_status", port=port)
    if before["running"]:
        raise ReleaseError("The editor already has a build running")
    started = editor_call("trigger_build", {"platformName": target["editorPlatform"]}, port)
    generation = started.get("generation")
    if generation is None:
        raise ReleaseError("Rebuild the editor to enable game build completion receipts")
    deadline = time.monotonic() + timeout
    last_message = None
    while time.monotonic() < deadline:
        status = editor_call("get_build_status", port=port)
        if status["generation"] != generation:
            raise ReleaseError("Another build replaced this request; refusing to publish unrelated output")
        if status.get("message") != last_message:
            last_message = status.get("message")
            print(last_message or "Building…", flush=True)
        if not status["running"]:
            if not status["succeeded"] or status["cancelled"]:
                raise ReleaseError("Game build failed: " + "; ".join(status.get("errors") or [last_message or "see editor log"]))
            if Path(status["outputDirectory"]).resolve() != source_path(base, target):
                raise ReleaseError(f"Editor exported to {status['outputDirectory']}, which differs from this target's source. "
                                   "Update publishing.json to point to the completed export.")
            return
        time.sleep(2)
    raise ReleaseError("Timed out waiting for the game export; the editor may still be building. No release was prepared.")


def run_build(base, targets, port=None, timeout=3600):
    for name, target in targets.items():
        commands = target.get("buildCommands", {}).get(host_platform())
        if not commands and target.get("editorPlatform"):
            build_in_editor(base, target, port, timeout)
            validate_payload(source_path(base, target), target)
            continue
        if not commands:
            raise ReleaseError(f"No {host_platform()} build recipe for {name}. Export in the editor and use prepare, "
                               "or set buildCommands for this host to a command that waits for the completed game export.")
        for command in commands:
            if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
                raise ReleaseError("Each build command must be an argument array, not a shell string")
            print(f"Building {name}: {command[0]}", flush=True)
            subprocess.run(command, cwd=base, check=True)
        validate_payload(source_path(base, target), target)


def vdf_string(value):
    value = str(value)
    if any(ord(c) < 32 for c in value):
        raise ReleaseError("SteamPipe values cannot contain control characters")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def steam_scripts(release, metadata, *, preview, branch=""):
    app_id = steam_id(metadata["steam"].get("appId"), "Steam App ID")
    if branch.lower() in ("default", "public"):
        raise ReleaseError("Promote public releases in Steamworks; automatic SetLive is for beta branches")
    if branch:
        identifier(branch, "Beta branch")
    scripts = release / "steampipe"
    scripts.mkdir(exist_ok=True)
    depots, seen = [], set()
    for name, target in metadata["targets"].items():
        depot = steam_id(target.get("depotId"), f"depot ID for {name}")
        if depot in seen:
            raise ReleaseError("Selected targets need distinct depot IDs; use the Linux depot for Deck too")
        seen.add(depot)
        text = ('"DepotBuild"\n{\n'
                f'  "DepotID" "{depot}"\n'
                f'  "ContentRoot" {vdf_string((release / "content" / name).as_posix())}\n'
                '  "FileMapping"\n  {\n    "LocalPath" "*"\n    "DepotPath" "."\n    "recursive" "1"\n  }\n}\n')
        (scripts / f"depot_{depot}.vdf").write_text(text, encoding="utf-8")
        depots.append(f'    "{depot}" "depot_{depot}.vdf"')
    output = release.parent / ".steampipe-cache" / app_id
    output.mkdir(parents=True, exist_ok=True)
    app = ('"AppBuild"\n{\n'
           f'  "AppID" "{app_id}"\n  "Desc" {vdf_string(metadata["release"])}\n'
           f'  "BuildOutput" {vdf_string(output.as_posix())}\n'
           f'  "Preview" "{1 if preview else 0}"\n')
    if branch and not preview:
        app += f'  "SetLive" {vdf_string(branch)}\n'
    app += '  "Depots"\n  {\n' + "\n".join(depots) + '\n  }\n}\n'
    path = scripts / ("preview.vdf" if preview else "upload.vdf")
    path.write_text(app, encoding="utf-8")
    return path


def prepare(base, config, targets, output, version):
    identifier(version, "Release name")
    output = output.resolve()
    final = output / version
    if final.exists():
        raise ReleaseError(f"Release already exists: {final}; choose a new release name")
    for target in targets.values():
        source = source_path(base, target)
        if output.is_relative_to(source) or source.is_relative_to(output):
            raise ReleaseError("Release storage and build output must not overlap")
        validate_payload(source, target)
    output.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".preparing-", dir=output))
    try:
        metadata = {"schemaVersion": 1, "release": version, "steam": config.get("steam", {}), "targets": {}}
        for name, target in targets.items():
            dest = staging / "content" / name
            shutil.copytree(source_path(base, target), dest, symlinks=True)
            # Never modify .app bundles after signing. Symbols and local Steam
            # overrides must be removed before signing/exporting a Mac app.
            for path in list(dest.rglob("*")):
                if path.name == "steam_appid.txt":
                    raise ReleaseError(f"Remove development steam_appid.txt before export: {path}")
                if path.is_file() and path.suffix.lower() in (".pdb", ".debug") and target["platform"] != "macos":
                    symbols = staging / "symbols" / name / path.relative_to(dest)
                    symbols.parent.mkdir(parents=True, exist_ok=True)
                    shutil.move(path, symbols)
            validate_payload(dest, target)
            entry = {k: target[k] for k in ("platform", "architecture", "executable", "gameConfig")}
            for key in ("depotId", "deviceProfile", "requiredFiles"):
                if key in target:
                    entry[key] = target[key]
            entry["files"] = tree_manifest(dest)
            metadata["targets"][name] = entry
        write_json(staging / "release.json", metadata)
        staging.rename(final)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    print(f"Prepared {final}")
    return final


def verify_release(release):
    metadata = read_json(release / "release.json")
    if metadata.get("schemaVersion") != 1 or not metadata.get("targets"):
        raise ReleaseError("Invalid release manifest")
    for name, target in metadata["targets"].items():
        identifier(name, "Target")
        root = release / "content" / name
        validate_payload(root, target)
        if tree_manifest(root) != target["files"]:
            raise ReleaseError(f"Release content changed after preparation: {name}. Prepare a new release.")
    return metadata


def steam_run(release, steamcmd, username, *, upload=False, branch=""):
    metadata = verify_release(release)
    script = steam_scripts(release, metadata, preview=not upload, branch=branch)
    if not steamcmd:
        if upload:
            raise ReleaseError("Upload requires --steamcmd (or STEAMCMD); upload.vdf has been prepared")
        print(f"Generated {script}; pass --steamcmd to run SteamCMD")
        return
    if not username:
        raise ReleaseError("Pass --username or set STEAM_BUILD_USER; SteamCMD handles login and Steam Guard")
    subprocess.run([steamcmd, "+login", username, "+run_app_build", str(script), "+quit"], check=True)


def ssh_command(host, key=None):
    if not re.fullmatch(r"(?:[A-Za-z0-9_.-]+@)?[A-Za-z0-9][A-Za-z0-9_.-]*", host):
        raise ReleaseError("SSH host must be a hostname or IPv4 address, optionally prefixed with user@")
    args = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"]
    if key:
        args += ["-i", str(Path(key).expanduser())]
    return args + [host]


REMOTE_INSTALL = r'''
import fcntl, hashlib, json, os, pathlib, sys, tarfile, uuid, shutil
root = pathlib.Path.home() / sys.argv[1]
version = sys.argv[2]
root.mkdir(parents=True, exist_ok=True)
lock = (root / '.deploy.lock').open('a')
fcntl.flock(lock, fcntl.LOCK_EX)
if not hasattr(tarfile, 'data_filter'):
    raise SystemExit('Deck deployment requires Python with tarfile.data_filter (Python 3.12+)')
releases = root / 'releases'
releases.mkdir(exist_ok=True)
final = releases / version
if final.exists():
    raise SystemExit('Release already installed; choose a new release name')
staging = releases / ('.incoming-' + uuid.uuid4().hex)
staging.mkdir()
try:
    with tarfile.open(fileobj=sys.stdin.buffer, mode='r|gz') as archive:
        for member in archive:
            path = pathlib.PurePosixPath(member.name)
            if path.is_absolute() or '..' in path.parts or not (member.isfile() or member.isdir() or member.issym()):
                raise ValueError('Unsafe archive entry: ' + member.name)
            dest = staging / member.name
            if not dest.resolve().is_relative_to(staging.resolve()):
                raise ValueError('Archive path escapes release')
            if member.issym():
                if pathlib.PurePosixPath(member.linkname).is_absolute() or not (dest.parent / member.linkname).resolve().is_relative_to(staging.resolve()):
                    raise ValueError('Unsafe archive symlink')
            archive.extract(member, staging, filter='data')
    manifest = json.loads((staging / '.deploy-manifest.json').read_text())
    for name, entry in manifest.items():
        path = staging / name
        if 'symlink' in entry:
            if not path.is_symlink() or os.readlink(path) != entry['symlink']:
                raise ValueError('Symlink mismatch: ' + name)
        else:
            checksum = hashlib.sha256()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1048576), b''):
                    checksum.update(block)
            if checksum.hexdigest() != entry['sha256']:
                raise ValueError('Checksum mismatch: ' + name)
    (staging / '.deploy-manifest.json').unlink()
    staging.rename(final)
    current = root / 'current'
    if current.exists() and not current.is_symlink():
        raise ValueError('current must be a release symlink')
    if current.is_symlink():
        previous = root / ('.previous-' + uuid.uuid4().hex)
        previous.symlink_to(os.readlink(current))
        os.replace(previous, root / 'previous')
    pending = root / ('.current-' + uuid.uuid4().hex)
    pending.symlink_to('releases/' + version)
    os.replace(pending, current)
    print('Installed and verified: ' + str(final))
    print('Steam shortcut should launch: ' + str(current / sys.argv[3]))
finally:
    if staging.exists():
        shutil.rmtree(staging)
'''


def deploy(release, target_name, host, remote_dir, key=None):
    relative(remote_dir)  # relative to the SSH user's home; never an arbitrary delete destination
    metadata = verify_release(release)
    target = metadata["targets"].get(target_name)
    if not target or (target["platform"], target["architecture"]) != ("linux", "x64"):
        raise ReleaseError("Select a Linux x64 target for Deck deployment")
    root = release / "content" / target_name
    version = identifier(metadata["release"] + "-" + target_name, "Deployment name")
    launcher = "launch.sh" if (root / "launch.sh").is_file() else target["executable"]
    with tempfile.TemporaryDirectory(prefix="ge-deploy-") as temp:
        temp = Path(temp)
        write_json(temp / ".deploy-manifest.json", target["files"])
        def permissions(member):
            # NTFS cannot carry Unix executable bits; explicitly restore the
            # Player and shell launchers when deploying from Windows.
            if member.name == target["executable"] or member.name == launcher or member.name.endswith(".sh"):
                member.mode |= 0o111
            return member
        with tarfile.open(temp / "payload.tar.gz", "w:gz", dereference=False) as archive:
            for child in sorted(root.iterdir()):
                archive.add(child, arcname=child.name, filter=permissions)
            archive.add(temp / ".deploy-manifest.json", arcname=".deploy-manifest.json")
        command = shlex.join(["python3", "-c", REMOTE_INSTALL, remote_dir, version, launcher])
        with (temp / "payload.tar.gz").open("rb") as stream:
            subprocess.run(ssh_command(host, key) + [command], stdin=stream, check=True)


REMOTE_ROLLBACK = r'''
import fcntl, os, pathlib, sys, uuid
root = pathlib.Path.home() / sys.argv[1]
lock = (root / '.deploy.lock').open('a')
fcntl.flock(lock, fcntl.LOCK_EX)
current, previous = root / 'current', root / 'previous'
if not current.is_symlink() or not previous.is_symlink() or not previous.resolve().is_dir():
    raise SystemExit('No previous deployed release is available')
old, restored = os.readlink(current), os.readlink(previous)
for name, value in (('current', restored), ('previous', old)):
    temp = root / ('.switch-' + uuid.uuid4().hex)
    temp.symlink_to(value)
    os.replace(temp, root / name)
print('Restored: ' + str(current.resolve()))
'''


REMOTE_SMOKE = r'''
import os, pathlib, signal, subprocess, sys, tempfile, time
root = (pathlib.Path.home() / sys.argv[1] / 'current').resolve(strict=True)
launcher = root / sys.argv[2]
if not launcher.is_file() or not launcher.resolve().is_relative_to(root):
    raise SystemExit('Missing launcher in the current release')
seconds = int(sys.argv[3])
logdir = pathlib.Path.home() / '.local/state/GameEngine/publishing'
logdir.mkdir(parents=True, exist_ok=True)
log = logdir / ('smoke-' + str(time.time_ns()) + '.log')
env = dict(os.environ, GE_PACKAGED_PLAYER='1')
env['LD_LIBRARY_PATH'] = str(root) + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
dependencies = subprocess.run(['ldd', str(root / 'Player')], env=env, capture_output=True, text=True)
if dependencies.returncode or 'not found' in dependencies.stdout or 'not found' in dependencies.stderr:
    raise SystemExit('Runtime dependencies failed:\n' + dependencies.stdout + dependencies.stderr)
with log.open('w') as output:
    process = subprocess.Popen([str(launcher)], cwd=root, env=env, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        pass
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
text = log.read_text(errors='replace')
print('Log: ' + str(log))
print(text[-16000:])
if 'Player: Initialized successfully' not in text or 'Runtime: Loaded scene' not in text:
    raise SystemExit('Smoke failed: expected initialization and startup scene markers were not observed')
if process.returncode not in (0, -signal.SIGTERM):
    raise SystemExit('Player exited unexpectedly: ' + str(process.returncode))
print('Startup smoke passed; controller, suspend/resume and frame pacing still require device testing')
'''


def remote_action(action, host, remote_dir, key=None, launcher="launch.sh", seconds=30):
    relative(remote_dir)
    relative(launcher)
    script = REMOTE_ROLLBACK if action == "rollback" else REMOTE_SMOKE
    command = shlex.join(["python3", "-c", script, remote_dir, launcher, str(seconds)])
    subprocess.run(ssh_command(host, key) + [command], check=True)


def init_config(path):
    if path.exists():
        raise ReleaseError(f"Configuration already exists: {path}")
    targets = {}
    for name, platform, arch, source, exe, config in (
        ("windows", "windows", "x64", "Build/Windows", "Player.exe", "game.config"),
        ("macos", "macos", "arm64", "Build/Mac", "My Game.app/Contents/MacOS/Player", "My Game.app/Contents/Resources/game.config"),
        ("linux", "linux", "x64", "Build/Linux", "Player", "game.config"),
    ):
        targets[name] = {"platform": platform, "architecture": arch, "source": source,
                         "executable": exe, "gameConfig": config, "depotId": None,
                         "editorPlatform": {"windows": "Windows", "macos": "Mac", "linux": "Linux"}[platform],
                         "requiredFiles": [{"windows": "Engine.dll", "linux": "libEngine.so",
                                            "macos": "My Game.app/Contents/Frameworks/libEngine.dylib"}[platform]],
                         "buildCommands": {}}
    path.parent.mkdir(parents=True, exist_ok=True)
    write_json(path, {"schemaVersion": 1, "steam": {"appId": None}, "targets": targets})
    print(f"Created {path}. Set output paths; add your Steam IDs when ready.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    init = sub.add_parser("init", help="Create a per-project publishing configuration")
    init.add_argument("path", type=Path, nargs="?", default=Path("publishing.json"))
    for name in ("check", "build", "prepare"):
        p = sub.add_parser(name)
        p.add_argument("--config", type=Path, default=Path("publishing.json"))
        p.add_argument("--target", action="append", help="Repeat for multiple targets; defaults to all configured targets")
        if name == "prepare":
            p.add_argument("--output", type=Path, default=Path("Releases"))
            p.add_argument("--release", required=True)
        if name == "build":
            p.add_argument("--editor-port", type=int)
            p.add_argument("--timeout", type=int, default=3600)
    for name in ("verify", "preview", "upload", "deploy"):
        p = sub.add_parser(name)
        p.add_argument("release", type=Path)
        if name in ("preview", "upload"):
            p.add_argument("--steamcmd", default=os.environ.get("STEAMCMD"))
            p.add_argument("--username", default=os.environ.get("STEAM_BUILD_USER"))
        if name == "upload":
            p.add_argument("--branch", default="", help="Optional beta branch; empty uploads without making live")
        if name == "deploy":
            p.add_argument("--target", default="linux")
            p.add_argument("--host", required=True)
            p.add_argument("--key")
            p.add_argument("--remote-dir", default="devkit-game/GameEngine", help="Release storage relative to the SSH user's home")
    for name in ("rollback", "smoke"):
        p = sub.add_parser(name)
        p.add_argument("--host", required=True)
        p.add_argument("--key")
        p.add_argument("--remote-dir", default="devkit-game/GameEngine")
        if name == "smoke":
            p.add_argument("--launcher", default="launch.sh")
            p.add_argument("--seconds", type=int, choices=range(5, 61), default=30, metavar="5..60")
    args = parser.parse_args(argv)
    try:
        if args.action == "init":
            init_config(args.path)
        elif args.action in ("check", "build", "prepare"):
            base, config = load_config(args.config)
            targets = selected_targets(config, args.target)
            if args.action == "build":
                run_build(base, targets, args.editor_port, args.timeout)
            elif args.action == "prepare":
                prepare(base, config, targets, args.output, args.release)
            else:
                for target in targets.values():
                    validate_payload(source_path(base, target), target)
                print("Build outputs validated")
        elif args.action == "verify":
            verify_release(args.release.resolve())
            print("Release checksums and package contents verified")
        elif args.action in ("preview", "upload"):
            steam_run(args.release.resolve(), args.steamcmd, args.username,
                      upload=args.action == "upload", branch=getattr(args, "branch", ""))
        elif args.action == "deploy":
            deploy(args.release.resolve(), args.target, args.host, args.remote_dir, args.key)
        else:
            remote_action(args.action, args.host, args.remote_dir, args.key,
                          getattr(args, "launcher", "launch.sh"), getattr(args, "seconds", 30))
        return 0
    except (ReleaseError, OSError, KeyError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
