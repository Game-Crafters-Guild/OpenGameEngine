#!/usr/bin/env python3
"""build_web_package.py over a stub engine module: the package's file list and manifest, the
size report, the refusal of a stub or incomplete module, and the version derivation.

    python Tools/Web/test_build_web_package.py

The package tests bundle the facade, so they need `npm ci` in Apps/WebLibrary/ts.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_web_package as package  # noqa: E402
import gepak  # noqa: E402

kSourceVersion = package.SourceVersion(package.kRepoRoot / "VERSION")
kHasBundler = (package.kFacadeDir / "node_modules" / "esbuild" / "bin" / "esbuild").is_file()


class StubTree:
    """Two module trees with empty glue, a one-file engine pack and a components.d.ts."""

    def __init__(self, root: Path):
        self.root = root
        self.dirs = {}
        for build in package.kBuilds:
            directory = root / build
            directory.mkdir()
            for name in (*package.ModuleFiles(build), package.kBindingName):
                (directory / name).write_bytes(b"")
            self.dirs[build] = directory
        self.pack = root / "engine.gepak"
        self.pack.write_bytes(gepak.Pack([gepak.PackEntry("Assets/stub.txt", b"stub")]))
        self.dts = root / "components.d.ts"
        self.dts.write_text("export {};\n")
        self.share = root / "share"
        for port, text in (("jolt", "Jolt license"), ("gtest", "test only")):
            (self.share / port).mkdir(parents=True)
            (self.share / port / "copyright").write_text(text)

    def Args(self, **overrides) -> argparse.Namespace:
        args = argparse.Namespace(st_dir=self.dirs["st"], mt_dir=self.dirs["mt"],
                                  component_dts=self.dts, engine_pack=self.pack,
                                  pack_build_tree=None, seed_project=None,
                                  vcpkg_share=self.share, version=kSourceVersion, allow_stub=True)
        for key, value in overrides.items():
            setattr(args, key, value)
        return args


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.tree = StubTree(Path(self.temp.name))
        self.out = Path(self.temp.name) / "package"

    @unittest.skipUnless(kHasBundler, "the facade's bundler is not installed: npm ci in Apps/WebLibrary/ts")
    def test_the_package_holds_every_file_and_a_loadable_manifest(self):
        package.Build(self.tree.Args(), self.out)
        files = sorted(path.relative_to(self.out).as_posix()
                       for path in self.out.rglob("*") if path.is_file())
        self.assertEqual([name for name in files if not name.startswith("examples/")],
                         package.PackageFiles())
        for page in ("model-viewer",):
            self.assertIn(f"examples/{page}/index.html", files)
            html = (self.out / "examples" / page / "index.html").read_text(encoding="utf-8")
            self.assertIn(package.kPackageImportTarget, html)
        notices = (self.out / package.kNoticesName).read_text(encoding="utf-8")
        self.assertIn("## jolt (vcpkg port, a dependency of the engine's wasm build)", notices)
        self.assertIn("Jolt license", notices)
        self.assertNotIn("test only", notices)
        self.assertIn("## coi-serviceworker", notices)

        manifest = json.loads((self.out / "package.json").read_text(encoding="utf-8"))
        self.assertEqual((manifest["name"], manifest["version"]), ("@openengine/web", kSourceVersion))
        self.assertEqual(manifest["exports"]["."]["default"], "./opengine.mjs")
        self.assertIs(manifest["sideEffects"], False)
        self.assertEqual(sorted(manifest["files"]),
                         sorted([*(name for name in package.PackageFiles() if name != "package.json"),
                                 "examples/"]))
        self.assertIn(f'var kPackageVersion = "{kSourceVersion}";',
                      (self.out / "opengine.mjs").read_text(encoding="utf-8"))

    @unittest.skipUnless(kHasBundler, "the facade's bundler is not installed: npm ci in Apps/WebLibrary/ts")
    def test_the_facade_carries_each_build_wasm_size(self):
        header = b"\0asm\1\0\0\0"
        (self.tree.dirs["st"] / "opengine-core.st.wasm").write_bytes(header + bytes(3))
        (self.tree.dirs["mt"] / "opengine-core.mt.wasm").write_bytes(header + bytes(5))
        package.Build(self.tree.Args(), self.out)
        self.assertIn("var kCoreWasmBytes = { st: 11, mt: 13 };",
                      (self.out / "opengine.mjs").read_text(encoding="utf-8"))

    @unittest.skipUnless(kHasBundler, "the facade's bundler is not installed: npm ci in Apps/WebLibrary/ts")
    def test_check_reports_each_build_download(self):
        self.tree.pack.write_bytes(gepak.Pack([gepak.PackEntry("Assets/zeros.bin", bytes(100_000))]))
        package.Build(self.tree.Args(), self.out)
        report = io.StringIO()
        with contextlib.redirect_stdout(report):
            self.assertEqual(package.Check(self.out, allowStub=True), [])
        rows = {line[:32].strip(): [int(value.replace(",", "")) for value in line[32:].split()]
                for line in report.getvalue().splitlines()[1:-1]}
        pack = rows["opengine-core.gepak"]
        self.assertEqual(pack[0], (self.out / "opengine-core.gepak").stat().st_size)
        self.assertLess(pack[2], pack[1])  # brotli beats gzip on a run of zeros
        for build in package.kBuilds:
            shared = [rows[name] for name in ("opengine.mjs", package.kBindingName,
                                              "opengine-core.gepak", *package.ModuleFiles(build))]
            self.assertEqual(rows[f"page download at {build}"],
                             [sum(row[column] for row in shared) for column in range(3)])

    @unittest.skipUnless(kHasBundler, "the facade's bundler is not installed: npm ci in Apps/WebLibrary/ts")
    def test_a_split_pack_joins_back_and_checks(self):
        self.tree.pack.write_bytes(gepak.Pack([gepak.PackEntry(f"Assets/{n}.bin", bytes(range(256)) * 40)
                                               for n in range(10)]))
        package.Build(self.tree.Args(), self.out)
        whole = (self.out / package.kEnginePackName).read_bytes()
        parts = package.SplitPack(self.out, partBytes=4096)
        self.assertFalse((self.out / package.kEnginePackName).exists())
        self.assertEqual(len(parts), -(-len(whole) // 4096))
        self.assertTrue(all((self.out / name).stat().st_size <= 4096 for name in parts))
        self.assertEqual(package.JoinPack(self.out), whole)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(package.Check(self.out, allowStub=True), [])
        (self.out / parts[1]).write_bytes(b"x" + (self.out / parts[1]).read_bytes()[1:])
        with self.assertRaises(package.PackageError):
            package.JoinPack(self.out)

    def test_the_site_holds_the_gallery_and_the_models_it_hosts(self):
        models = Path(self.temp.name) / "models"
        models.mkdir()
        (models / "Helmet.glb").write_bytes(b"glTF")
        site = Path(self.temp.name) / "site"
        self.assertEqual(package.WriteSite(site, models), site / "package")
        self.assertEqual((site / "models" / "Helmet.glb").read_bytes(), b"glTF")
        self.assertIn(package.kSiteExamplesLink, (site / "index.html").read_text(encoding="utf-8"))
        (models / "Helmet.glb").unlink()
        with self.assertRaises(package.PackageError):
            package.WriteSite(site, models)

    def test_the_site_pack_drops_the_particle_programs_only(self):
        self.out.mkdir()
        entries = [gepak.PackEntry(".workspace/.Cache/Shaders/aa/program.shaderpkg", b"mesh program"),
                   gepak.PackEntry(".workspace/.Cache/Shaders/bb/program.shaderpkg", b"out vParticleBasis"),
                   gepak.PackEntry("Assets/Shaders/Adapters/adapter_vertex.glsl", b"out vParticleBasis")]
        (self.out / package.kEnginePackName).write_bytes(gepak.Pack(entries))
        self.assertEqual(package.StripParticlePrograms(self.out), (1, 1))
        kept = [entry.Path for entry in gepak.Unpack((self.out / package.kEnginePackName).read_bytes())]
        self.assertEqual(kept, [entries[0].Path, entries[2].Path])

    def test_notices_carry_complete_toolchain_texts_and_example_credits(self):
        self.out.mkdir()
        package.WriteNotices(self.out, self.tree.share)
        notices = (self.out / package.kNoticesName).read_text(encoding="utf-8")
        for relative in ("Tools/Web/licenses/toolchain-notices.md",
                         "Tools/Web/licenses/rust-dependency-notices.md",
                         "Tools/Web/licenses/CC-BY-4.0.txt",
                         "Tools/Web/licenses/CC0-1.0.txt",
                         "Apps/WebLibrary/examples/ASSET_PROVENANCE.md"):
            with self.subTest(source=relative):
                source = (package.kRepoRoot / relative).read_text(encoding="utf-8")
                self.assertIn(source, notices)
        self.assertIn("wgpu-native 29.0.1.1", notices)
        self.assertIn("Naga CLI 29.0.4", notices)
        self.assertIn("Tint / Dawn 36cf1fae0cd8", notices)
        self.assertIn("ABeautifulGame.glb", notices)

    def test_a_stub_module_is_refused_by_name(self):
        with self.assertRaises(package.PackageError) as refusal:
            package.CopyModules(self.tree.dirs, self.out, allowStub=False)
        for build in package.kBuilds:
            for name in package.ModuleFiles(build):
                self.assertIn(f"{self.tree.dirs[build] / name} is empty (a stub module", str(refusal.exception))

    def test_missing_glue_is_refused_even_with_allow_stub(self):
        (self.tree.dirs["mt"] / "opengine-core.mt.js").unlink()
        with self.assertRaises(package.PackageError) as refusal:
            package.CopyModules(self.tree.dirs, self.out, allowStub=True)
        self.assertIn(f"{self.tree.dirs['mt'] / 'opengine-core.mt.js'} is missing", str(refusal.exception))

    def test_the_version_comes_from_the_version_file_and_the_release_tag(self):
        self.assertEqual(package.PackageVersion("v2026.10.0", "abc1234567", "2026.10.0"), "2026.10.0")
        self.assertEqual(package.PackageVersion("v2026.10.0-alpha.1", "abc1234567", "2026.10.0-alpha.1"),
                         "2026.10.0-alpha.1")
        self.assertEqual(package.PackageVersion(None, "abc1234567", "2026.10.0-alpha.4"),
                         "2026.10.0-alpha.4-dev.abc1234567")
        for tag in ("v12", "v2026.10.0-rc.1", "release-2026.10.0"):
            with self.assertRaises(package.PackageError):
                package.PackageVersion(tag, "abc1234567", "2026.10.0")

    def test_a_release_tag_that_differs_from_the_version_file_is_refused_naming_both(self):
        with self.assertRaises(package.PackageError) as refusal:
            package.PackageVersion("v2026.10.0-alpha.3", "abc1234567", "2026.10.0-alpha.4")
        self.assertIn("v2026.10.0-alpha.3", str(refusal.exception))
        self.assertIn("2026.10.0-alpha.4", str(refusal.exception))

    def test_a_version_argument_that_differs_from_the_version_file_is_refused_naming_both(self):
        with self.assertRaises(package.PackageError) as refusal:
            package.Build(self.tree.Args(version="2000.1.0-alpha.1"), self.out)
        self.assertIn("2000.1.0-alpha.1", str(refusal.exception))
        self.assertIn(kSourceVersion, str(refusal.exception))

    def test_the_repository_version_file_holds_a_release_version(self):
        self.assertTrue(package.kReleaseTag.match(f"v{package.SourceVersion(package.kRepoRoot / 'VERSION')}"))

    def test_a_prerelease_publishes_under_its_channel_never_latest(self):
        self.assertEqual(package.PrereleaseChannel("2026.10.0-alpha.1"), "alpha")
        self.assertEqual(package.PrereleaseChannel("2026.10.0-beta.2"), "beta")
        self.assertIsNone(package.PrereleaseChannel("2026.10.0"))
        self.out.mkdir()
        package.WritePackageJson(self.out, "2026.10.0-alpha.1")
        manifest = json.loads((self.out / "package.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["publishConfig"], {"tag": "alpha"})
        package.WritePackageJson(self.out, "2026.10.0")
        self.assertNotIn("publishConfig", json.loads((self.out / "package.json").read_text(encoding="utf-8")))

if __name__ == "__main__":
    unittest.main()
