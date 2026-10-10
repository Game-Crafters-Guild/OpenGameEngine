#!/usr/bin/env python3
"""Device-free release contract tests; run identically on Windows, macOS and Linux."""
import copy
import io
import json
import os
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Tools/Scripts"))
import publish_game as pub
import build_deck
import deck_template


def binary(platform="linux", arch="x64"):
    data = bytearray(128)
    if platform == "linux":
        data[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", data, 18, 62 if arch == "x64" else 183)
    elif platform == "windows":
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 80)
        data[80:84] = b"PE\0\0"
        struct.pack_into("<H", data, 84, 0x8664 if arch == "x64" else 0xAA64)
    else:
        data[:4] = b"\xcf\xfa\xed\xfe"
        struct.pack_into("<I", data, 4, 0x1000007 if arch == "x64" else 0x100000c)
    return data


class PublishingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="game publish ' test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / "Build" / "Linux"
        self.source.mkdir(parents=True)
        (self.source / "Player").write_bytes(binary())
        (self.source / "Player").chmod(0o755)
        for name in ("Scenes/Start.scene", "RenderPipelines/Game.rendergraph", "Shaders/main.shaderpkg"):
            path = self.source / "Assets" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture", encoding="utf-8")
        (self.source / "Assets/.assetmanifest").write_text("{}\n", encoding="utf-8")
        pub.write_json(self.source / "game.config", {"startupScene": "Scenes/Start.scene",
                       "renderPipeline": "RenderPipelines/Game.rendergraph", "scriptAssemblyPath": ""})
        self.target = {"platform": "linux", "architecture": "x64", "source": "Build/Linux",
                       "executable": "Player", "gameConfig": "game.config", "depotId": 123002}
        self.config = {"schemaVersion": 1, "steam": {"appId": 123000}, "targets": {"linux": self.target}}

    def prepare(self, version="test-1"):
        return pub.prepare(self.root, self.config, self.config["targets"], self.root / "Releases", version)

    def test_release_roundtrip_and_tamper_detection(self):
        release = self.prepare()
        pub.verify_release(release)
        (release / "content/linux/Assets/Scenes/Start.scene").write_text("different")
        with self.assertRaisesRegex(pub.ReleaseError, "changed after preparation"):
            pub.verify_release(release)

    def test_release_is_never_overwritten(self):
        release = self.prepare()
        manifest = (release / "release.json").read_bytes()
        with self.assertRaisesRegex(pub.ReleaseError, "already exists"):
            self.prepare()
        self.assertEqual(manifest, (release / "release.json").read_bytes())

    def test_failed_prepare_keeps_existing_release(self):
        release = self.prepare()
        (self.source / "game.config").unlink()
        with self.assertRaises(OSError):
            self.prepare("test-2")
        self.assertFalse((self.root / "Releases/test-2").exists())
        pub.verify_release(release)

    def test_missing_scene_shader_manifest_and_assembly(self):
        for name in ("Assets/Scenes/Start.scene", "Assets/Shaders/main.shaderpkg", "Assets/.assetmanifest"):
            with self.subTest(name=name):
                path = self.source / name
                data = path.read_bytes()
                path.unlink()
                with self.assertRaises(pub.ReleaseError):
                    pub.validate_payload(self.source, self.target)
                path.write_bytes(data)
        config = pub.read_json(self.source / "game.config")
        config["scriptAssemblyPath"] = "Managed/GameEngine.Scripts.dll"
        pub.write_json(self.source / "game.config", config)
        with self.assertRaisesRegex(pub.ReleaseError, "gameplay assembly"):
            pub.validate_payload(self.source, self.target)

    def test_gameplay_sources_cannot_silently_lose_scripts(self):
        (self.source / "Assets/Game.cs").write_text("class Game {}")
        with self.assertRaisesRegex(pub.ReleaseError, "no compiled gameplay"):
            pub.validate_payload(self.source, self.target)

    def test_binary_targets(self):
        path = self.source / "Player"
        for platform in ("windows", "linux", "macos"):
            for arch in ("x64", "arm64"):
                path.write_bytes(binary(platform, arch))
                self.assertEqual(pub.binary_target(path), (platform, arch))
        with self.assertRaisesRegex(pub.ReleaseError, "Wrong Player"):
            pub.validate_payload(self.source, self.target)

    def test_foreign_runtime_library_is_rejected(self):
        (self.source / "libplugin.so").write_bytes(binary("linux", "arm64"))
        with self.assertRaisesRegex(pub.ReleaseError, "runtime library architecture"):
            pub.validate_payload(self.source, self.target)

    def mac_bundle(self, app="Game.app"):
        # The exported layout: code in Contents/MacOS, everything else in Contents/Resources.
        root = self.root / "Mac"
        content = root / app / "Contents/Resources"
        shutil.copytree(self.source, content)
        (content / "Player").unlink()
        executable = root / app / "Contents/MacOS/Player"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(binary("macos", "arm64"))
        executable.chmod(0o755)
        target = {**self.target, "platform": "macos", "architecture": "arm64",
                  "executable": f"{app}/Contents/MacOS/Player", "gameConfig": f"{app}/Contents/Resources/game.config"}
        return root, content, target

    def test_mac_managed_assemblies_in_bundle_resources(self):
        root, content, target = self.mac_bundle()
        assembly = content / "Managed/GameEngine.Scripts.dll"
        assembly.parent.mkdir(parents=True)
        assembly.write_bytes(b"managed fixture")
        config = pub.read_json(content / "game.config")
        config["scriptAssemblyPath"] = "Managed/GameEngine.Scripts.dll"
        pub.write_json(content / "game.config", config)
        pub.validate_payload(root, target)

    def test_init_mac_target_matches_the_exported_bundle(self):
        config_path = self.root / "Init" / "publishing.json"
        pub.init_config(config_path)
        mac = pub.read_json(config_path)["targets"]["macos"]
        root, _, _ = self.mac_bundle("My Game.app")
        pub.validate_payload(root, {**self.target, **{key: mac[key] for key in
                                                      ("platform", "architecture", "executable", "gameConfig")}})

    def test_mac_native_aot_gameplay_beside_the_executable(self):
        root, content, target = self.mac_bundle()
        (content / "Assets/Game.cs").write_text("class Game {}")
        with self.assertRaisesRegex(pub.ReleaseError, "no compiled gameplay"):
            pub.validate_payload(root, target)
        (root / "Game.app/Contents/MacOS/GameEngine.Scripts.native.dylib").write_bytes(binary("macos", "arm64"))
        pub.validate_payload(root, target)

    def test_path_escape(self):
        for name in ("../escape", "/absolute", "C:/Windows", "a\\b", "./Player"):
            with self.subTest(name=name), self.assertRaises(pub.ReleaseError):
                pub.relative(name)
        target = {**self.target, "executable": "../Player"}
        with self.assertRaises(pub.ReleaseError):
            pub.validate_payload(self.source, target)

    def test_outside_symlink_is_rejected(self):
        if os.name == "nt":
            self.skipTest("Windows symlink creation requires developer mode")
        (self.source / "outside").symlink_to(self.root)
        with self.assertRaises(pub.ReleaseError):
            self.prepare()

    def test_nested_source_and_release_rejected(self):
        with self.assertRaisesRegex(pub.ReleaseError, "overlap"):
            pub.prepare(self.root, self.config, self.config["targets"], self.source / "releases", "v1")

    def test_symbols_are_separate(self):
        (self.source / "Player.pdb").write_bytes(b"symbols")
        release = self.prepare()
        self.assertTrue((release / "symbols/linux/Player.pdb").is_file())
        self.assertFalse((release / "content/linux/Player.pdb").exists())
        self.assertTrue((self.source / "Player.pdb").is_file())

    def test_local_appid_rejected(self):
        (self.source / "steam_appid.txt").write_text("480")
        with self.assertRaisesRegex(pub.ReleaseError, "steam_appid"):
            self.prepare()

    def test_steam_preview_and_upload_are_distinct(self):
        release = self.prepare()
        manifest = pub.verify_release(release)
        preview = pub.steam_scripts(release, manifest, preview=True, branch="beta").read_text()
        upload = pub.steam_scripts(release, manifest, preview=False, branch="beta").read_text()
        self.assertIn('"Preview" "1"', preview)
        self.assertNotIn('"SetLive"', preview)
        self.assertIn('"Preview" "0"', upload)
        self.assertIn('"SetLive" "beta"', upload)
        self.assertNotIn("symbols", (release / "steampipe/depot_123002.vdf").read_text())
        with self.assertRaises(pub.ReleaseError):
            pub.steam_scripts(release, manifest, preview=False, branch="default")

    def test_ids_optional_until_steam_action(self):
        self.config["steam"]["appId"] = None
        release = self.prepare()
        with self.assertRaisesRegex(pub.ReleaseError, "App ID"):
            pub.steam_scripts(release, pub.verify_release(release), preview=True)

    def test_upload_requires_tool_and_no_password_argument(self):
        release = self.prepare()
        with self.assertRaises(pub.ReleaseError):
            pub.steam_run(release, None, "builder", upload=True)
        with mock.patch.object(pub.subprocess, "run") as run:
            pub.steam_run(release, "steamcmd", "builder", upload=True)
            args = run.call_args.args[0]
            self.assertEqual(args[:3], ["steamcmd", "+login", "builder"])
            self.assertEqual(args[3], "+run_app_build")

    def test_host_build_recipes_wait_and_validate(self):
        self.target["buildCommands"] = {"windows": [["builder.exe", "project with spaces"]]}
        with mock.patch.object(pub, "host_platform", return_value="windows"), mock.patch.object(pub.subprocess, "run") as run:
            pub.run_build(self.root, self.config["targets"])
            self.assertEqual(run.call_args.args[0], ["builder.exe", "project with spaces"])
            self.assertTrue(run.call_args.kwargs["check"])
        with mock.patch.object(pub, "host_platform", return_value="linux"), self.assertRaises(pub.ReleaseError):
            pub.run_build(self.root, self.config["targets"])

    def test_editor_build_waits_for_its_completion_receipt(self):
        self.target["editorPlatform"] = "Linux"
        replies = [{"workspaceRoot": str(self.root)}, {"running": False}, {"generation": 7},
                   {"generation": 7, "running": True, "message": "Compiling"},
                   {"generation": 7, "running": False, "succeeded": True, "cancelled": False,
                    "outputDirectory": str(self.source), "message": "Done"}]
        with mock.patch.object(pub, "editor_call", side_effect=replies), mock.patch.object(pub.time, "sleep") as sleep:
            pub.build_in_editor(self.root, self.target)
            sleep.assert_called_once_with(2)

    def test_editor_failure_and_superseded_build_never_publish(self):
        self.target["editorPlatform"] = "Linux"
        for status in ({"generation": 8}, {"generation": 7, "running": False, "succeeded": False,
                                            "cancelled": False, "errors": ["script compile failed"]}):
            replies = [{"workspaceRoot": str(self.root)}, {"running": False}, {"generation": 7}, status]
            with mock.patch.object(pub, "editor_call", side_effect=replies), self.assertRaises(pub.ReleaseError):
                pub.build_in_editor(self.root, self.target)

    def test_wrong_editor_project_is_rejected_before_build(self):
        with mock.patch.object(pub, "editor_call", return_value={"workspaceRoot": str(self.root / "different")}) as call:
            with self.assertRaisesRegex(pub.ReleaseError, "different project"):
                pub.build_in_editor(self.root, {**self.target, "editorPlatform": "Linux"})
            self.assertEqual(call.call_count, 1)

    def test_incomplete_package_index_rejected(self):
        (self.source / "Packages").mkdir()
        pub.write_json(self.source / "Packages/packages.index", {"packages": [{"name": "water", "alias": "water"}]})
        with self.assertRaisesRegex(pub.ReleaseError, "Incomplete staged package"):
            pub.validate_payload(self.source, self.target)

    def test_invalid_ssh_host_cannot_be_an_option(self):
        for host in ("-oProxyCommand=evil", "deck@host;echo evil", "deck@host\n"):
            with self.assertRaises(pub.ReleaseError):
                pub.ssh_command(host)

    def test_template_identity_tracks_dirty_sources_and_runtime(self):
        repo = self.root / "repo"
        (repo / "Engine").mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        source = repo / "Engine/Test.cpp"
        source.write_text("version 1")
        (self.source / "libEngine.so").write_bytes(binary())
        identity = deck_template.fingerprint(repo)
        deck_template.seal(repo, self.source, identity)
        deck_template.verify(identity, self.source)
        source.write_text("version 2")
        with self.assertRaisesRegex(pub.ReleaseError, "stale"):
            deck_template.verify(deck_template.fingerprint(repo), self.source)
        with self.assertRaisesRegex(pub.ReleaseError, "changed during compilation"):
            deck_template.seal(repo, self.source, identity)
        with self.assertRaisesRegex(pub.ReleaseError, "no engine source identity"):
            deck_template.verify("", self.source)
        source.write_text("version 1")
        (self.source / "libEngine.so").write_bytes(b"different runtime")
        with self.assertRaisesRegex(pub.ReleaseError, "changed after compilation"):
            deck_template.verify(identity, self.source)

    def test_editor_stamp_matches_fingerprint_and_rewrites_only_on_change(self):
        repo = self.root / "repo"
        (repo / "Engine").mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        (repo / "Engine/Test.cpp").write_text("version 1")
        stamp_file = self.root / "editor/Tools/SteamDeck/engine-source.fingerprint"
        deck_template.stamp(repo, stamp_file)
        self.assertEqual(stamp_file.read_text(encoding="utf-8"), deck_template.fingerprint(repo))
        written = stamp_file.stat().st_mtime_ns
        os.utime(stamp_file, ns=(written - 10**9, written - 10**9))
        deck_template.stamp(repo, stamp_file)
        self.assertEqual(stamp_file.stat().st_mtime_ns, written - 10**9)

    def test_editor_stamp_without_checkout_is_empty(self):
        not_a_checkout = self.root / "sources"
        not_a_checkout.mkdir()
        stamp_file = self.root / "engine-source.fingerprint"
        deck_template.stamp(not_a_checkout, stamp_file)
        self.assertEqual(stamp_file.read_text(encoding="utf-8"), "")

    def test_promotion_failure_restores_both_old_outputs(self):
        candidate, output = self.root / "candidate", self.root / "output"
        for root, data in ((candidate, b"new"), (output, b"old")):
            (root / "steamdeck-player").mkdir(parents=True)
            (root / "steamdeck-player/Player").write_bytes(data)
            (root / "steamdeck-player.tar.gz").write_bytes(data)
        real_rename = Path.rename
        def fail_archive(path, target):
            if path == candidate / "steamdeck-player.tar.gz":
                raise OSError("simulated promotion failure")
            return real_rename(path, target)
        with mock.patch.object(Path, "rename", fail_archive), self.assertRaises(OSError):
            build_deck.promote(candidate, output)
        self.assertEqual((output / "steamdeck-player/Player").read_bytes(), b"old")
        self.assertEqual((output / "steamdeck-player.tar.gz").read_bytes(), b"old")

    @unittest.skipIf(os.name == "nt", "The Docker shell entry runs in WSL on Windows")
    def test_deck_preflight_preserves_last_build(self):
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        docker = bin_dir / "docker"
        docker.write_text("#!/bin/sh\nexit 0\n")
        docker.chmod(0o755)
        output = self.root / "last-deck-build"
        (output / "steamdeck-player").mkdir(parents=True)
        sentinel = output / "steamdeck-player/Player"
        sentinel.write_bytes(b"last successful game")
        script = Path(pub.__file__).parent / "build-steamdeck-docker.sh"
        if not script.is_file():
            self.skipTest("Source shell script is not part of this remote unit-test bundle")
        env = {**os.environ, "PATH": str(bin_dir) + os.pathsep + os.environ["PATH"],
               "GE_STEAMDECK_HOST_DIST_DIR": str(output), "GE_STEAMDECK_PROJECT_ROOT": str(self.root),
               "VCPKG_MAX_CONCURRENCY": "1", "JOBS": "1"}
        subprocess.run(["bash", str(script), "--check"], env=env, check=True, capture_output=True)
        self.assertEqual(sentinel.read_bytes(), b"last successful game")

    @unittest.skipIf(os.name == "nt", "Remote receiver runs on SteamOS")
    @unittest.skipUnless(hasattr(tarfile, "data_filter"), "Remote receiver requires modern tarfile extraction filters")
    def test_deck_receiver_verifies_then_switches_and_rolls_back(self):
        def install(version, corrupt=False):
            data = b"new build " + version.encode()
            buffer = io.BytesIO()
            checksum = pub.hashlib.sha256(data).hexdigest()
            manifest = json.dumps({"Player": {"sha256": "wrong" if corrupt else checksum}}).encode()
            with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
                for name, value in (("Player", data), (".deploy-manifest.json", manifest)):
                    entry = tarfile.TarInfo(name)
                    entry.size = len(value)
                    entry.mode = 0o755
                    archive.addfile(entry, io.BytesIO(value))
            return subprocess.run([sys.executable, "-c", pub.REMOTE_INSTALL, "deck", version, "Player"],
                                  input=buffer.getvalue(), env={**os.environ, "HOME": str(self.root)}, capture_output=True)
        self.assertEqual(install("v1").returncode, 0)
        self.assertNotEqual(install("broken", corrupt=True).returncode, 0)
        self.assertEqual(os.readlink(self.root / "deck/current"), "releases/v1")
        self.assertEqual(install("v2").returncode, 0)
        subprocess.run([sys.executable, "-c", pub.REMOTE_ROLLBACK, "deck"],
                       env={**os.environ, "HOME": str(self.root)}, capture_output=True, check=True)
        self.assertEqual(os.readlink(self.root / "deck/current"), "releases/v1")


if __name__ == "__main__":
    unittest.main()
