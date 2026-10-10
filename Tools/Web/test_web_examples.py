#!/usr/bin/env python3
"""The web library's example rules (design D6), checked over Apps/WebLibrary/examples.

Each example is a folder: index.html, main.js with the engine calls, and the page's own
controls in other .js and .css files beside it. main.js stays under 60 lines; its first comment
states the license of the content (`// License:`) and what it needs (`// Needs:`); the gallery
lists the example with main.js's line count and the line count of the page and its controls;
and every asset a script names (`'<name>.glb'`) has a row in ASSET_PROVENANCE.md naming the
example. The code the pages share in examples/shared/ is counted once, in the gallery's footer.

    python Tools/Web/test_web_examples.py
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

kLibraryDir = Path(__file__).resolve().parents[2] / "Apps" / "WebLibrary"
kMaxLines = 60
# A page starts on this status, with a download bar that Engine.create's onProgress fills and
# counts as a percentage while the engine downloads.
kEngineDownload = ('<output name="status">Loading the engine...</output>', '<progress class="download" hidden></progress>')
kCommentLabel = re.compile(r"^// [A-Z][A-Za-z]*:")
# The licenses a page credits on screen, as the panel names them.
kCreditedLicense = re.compile(r"CC BY(?:-NC)? 4\.0")
kAssetName = re.compile(r"[/'\"`]([^/'\"`\s]+\.(?:glb|gltf|png|jpe?g|hdr|ktx2))['\"`]")


def LineCount(path: Path) -> int:
    return len(path.read_text(encoding="utf-8").splitlines())


def LabelledLines(comment: list[str], label: str) -> list[str]:
    """The comment lines of `label` (`// License:`): its line and the lines that continue it."""
    lines = []
    for line in comment:
        if lines and kCommentLabel.match(line):
            break
        if lines or line.startswith(label):
            lines.append(line)
    return lines


def PanelFiles(example: Path) -> list[Path]:
    """The page and its controls: everything in the example but main.js."""
    return sorted(path for path in example.iterdir()
                  if path.suffix in (".html", ".js", ".css") and path.name != "main.js")


def ExampleProblems(example: Path, gallery: str, provenance: str) -> list[str]:
    name = example.name
    problems = []
    lines = LineCount(example / "main.js")
    if lines >= kMaxLines:
        problems.append(f"{name}/main.js is {lines} lines; an example's engine calls stay under {kMaxLines}")
    panelLines = sum(LineCount(path) for path in PanelFiles(example))
    html = (example / "index.html").read_text(encoding="utf-8")
    for piece in kEngineDownload:
        if piece not in html:
            problems.append(f"{name}/index.html does not start with the status and the download bar: no '{piece}'")

    script = (example / "main.js").read_text(encoding="utf-8").splitlines()
    firstComment = []
    for line in script:
        if not line.startswith("//"):
            break
        firstComment.append(line)
    for label in ("// License:", "// Needs:"):
        if not any(line.startswith(label) for line in firstComment):
            problems.append(f"{name}/main.js's first comment has no `{label}` line")
    licenseText = " ".join(LabelledLines(firstComment, "// License:"))
    credited = set()
    for path in PanelFiles(example):
        if path.suffix == ".js":
            credited |= set(kCreditedLicense.findall(path.read_text(encoding="utf-8")))
    for license in sorted(credited):
        if license not in licenseText:
            problems.append(f"{name}/main.js's License line does not name {license}, which the page credits")

    entry = re.search(rf'href="\.\./examples/{re.escape(name)}/index\.html".*?'
                      r'data-lines="(\d+)" data-panel-lines="(\d+)"', gallery, re.DOTALL)
    if entry is None:
        problems.append(f"the gallery does not list {name} with its two line counts")
    else:
        if int(entry.group(1)) != lines:
            problems.append(f"the gallery says {name} is {entry.group(1)} lines; it is {lines}")
        if int(entry.group(2)) != panelLines:
            problems.append(f"the gallery says {name}'s page and panel are {entry.group(2)} lines; "
                            f"they are {panelLines}")

    rows = [row for row in provenance.splitlines() if row.startswith("|")]
    scripts = "\n".join(path.read_text(encoding="utf-8") for path in example.glob("*.js"))
    for asset in sorted(set(kAssetName.findall(scripts))):
        if not any(asset in row and f"`{name}`" in row for row in rows):
            problems.append(f"ASSET_PROVENANCE.md has no row for {asset} used by `{name}`")
    return problems


def LibraryProblems(library: Path) -> list[str]:
    examples = sorted(path.parent for path in (library / "examples").glob("*/index.html"))
    gallery = (library / "gallery" / "index.html").read_text(encoding="utf-8")
    provenance = (library / "examples" / "ASSET_PROVENANCE.md").read_text(encoding="utf-8")
    problems = [] if examples else ["no examples found"]
    for example in examples:
        problems += ExampleProblems(example, gallery, provenance)
    # The code every page imports from examples/shared/ is counted once, in the gallery's footer.
    sharedLines = sum(LineCount(path) for path in (library / "examples" / "shared").glob("*.js"))
    shared = re.search(r'data-shared-lines="(\d+)"', gallery)
    if shared is None or int(shared.group(1)) != sharedLines:
        problems.append(f"the gallery says the shared page code is {shared and shared.group(1)} lines; it is {sharedLines}")
    return problems


# The shared fit on a page with no room beside the open panel (its strip reaches above the canvas's
# top), a zoom ratio that is not a number, and a model with no extent: printed as JSON.
kNoRoomScript = """
const { fitToFreeArea } = await import(process.argv[1]);
globalThis.document = { querySelector: (selector) => selector === '.panel'
    ? { open: true, getBoundingClientRect: () => ({ left: 0, top: -50, right: 568, bottom: 108, width: 568, height: 158 }) }
    : null };
const canvas = { clientWidth: 568, clientHeight: 120 };
const box = { center: { x: 0, y: 1, z: 0 }, size: { x: 2, y: 2, z: 2 } };
const level = { x: 0, y: 1, z: 0, w: 0 };
console.log(JSON.stringify([fitToFreeArea(canvas, box, 60, level), fitToFreeArea(canvas, box, 60, level, NaN),
    fitToFreeArea(canvas, { center: box.center, size: { x: 0, y: 0, z: 0 } }, 60, level)]));
"""

# A model stood the viewer's way: one with a box (4 wide, 1 tall, 2 deep, a scale of 1) and one with
# nothing to draw (no box): the scale each ends with, or null; printed as JSON.
kStandardiseScript = """
const { standardise } = await import(process.argv[1]);
const vector = () => ({ x: 1, y: 1, z: 1, set(x, y, z) { Object.assign(this, { x, y, z }); } });
const quaternion = () => ({ x: 0, y: 0, z: 0, w: 1, set(x, y, z, w) { Object.assign(this, { x, y, z, w }); } });
const modelWith = (bounds) => ({ bounds, transform: { scale: vector(), quaternion: quaternion(), position: vector(), rotateWorldY() {} } });
const sized = modelWith({ center: { x: 0, y: 0.5, z: 0 }, size: { x: 4, y: 1, z: 2 } });
const empty = modelWith(null);
console.log(JSON.stringify([standardise(sized) && sized.transform.scale, standardise(empty)]));
"""


class WebExamplesTests(unittest.TestCase):
    def test_every_example_meets_the_rules(self):
        self.assertEqual(LibraryProblems(kLibraryDir), [])

    @unittest.skipUnless(shutil.which("node"), "node is not on PATH")
    def test_a_fit_with_no_room_no_zoom_or_no_extent_stays_finite(self):
        script = (kLibraryDir / "examples" / "shared" / "page-status.js").resolve().as_uri()
        result = subprocess.run(["node", "--input-type=module", "-e", kNoRoomScript, script],
                                capture_output=True, text=True, check=True)
        fits = json.loads(result.stdout)
        for fit in fits:
            numbers = [fit["distance"], fit["fitted"], *fit["target"].values()]
            self.assertTrue(all(isinstance(n, (int, float)) for n in numbers), fit)
        self.assertGreater(fits[0]["distance"], 0)
        self.assertEqual(fits[1]["distance"], fits[0]["distance"])   # no zoom: the fit's own distance
        self.assertEqual(fits[2]["target"], {"x": 0, "y": 1, "z": 0})   # no extent: the model's centre

    @unittest.skipUnless(shutil.which("node"), "node is not on PATH")
    def test_every_model_is_sized_to_the_standard_reach_and_one_with_no_box_is_left_alone(self):
        script = (kLibraryDir / "examples" / "shared" / "page-status.js").resolve().as_uri()
        result = subprocess.run(["node", "--input-type=module", "-e", kStandardiseScript, script],
                                capture_output=True, text=True, check=True)
        scale, empty = json.loads(result.stdout)
        self.assertEqual({axis: scale[axis] for axis in "xyz"}, {"x": 0.5, "y": 0.5, "z": 0.5})   # 4 m long -> 2 m
        self.assertIsNone(empty)

    # Each rule, broken once in a copy of the library, is reported.
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.library = Path(self.temp.name)
        for folder in ("examples", "gallery"):
            shutil.copytree(kLibraryDir / folder, self.library / folder)
        self.script = self.library / "examples" / "model-viewer" / "main.js"

    def Edit(self, path: Path, old: str, new: str) -> None:
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def assertReported(self, fragment: str) -> None:
        problems = LibraryProblems(self.library)
        self.assertTrue(any(fragment in problem for problem in problems), problems)

    def test_an_example_of_60_lines_is_reported(self):
        lines = LineCount(self.script)
        self.script.write_text(self.script.read_text(encoding="utf-8") + "\n" * (kMaxLines - lines),
                               encoding="utf-8")
        self.assertReported("engine calls stay under 60")

    def test_a_first_comment_without_its_license_is_reported(self):
        self.Edit(self.script, "// License:", "// Licence")
        self.assertReported("no `// License:` line")

    def test_a_stale_gallery_line_count_is_reported(self):
        self.script.write_text(self.script.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        self.assertReported("the gallery says model-viewer is")

    def test_an_asset_without_a_provenance_row_is_reported(self):
        self.Edit(self.script.with_name("panel.js"), "'Lantern.glb'", "'SheenChair.glb'")
        self.assertReported("no row for SheenChair.glb used by `model-viewer`")

    def test_a_license_line_missing_a_credited_license_is_reported(self):
        self.Edit(self.script, "CC0 1.0, CC BY 4.0 and CC BY-NC 4.0", "CC0 1.0 and CC BY 4.0")
        self.assertReported("does not name CC BY-NC 4.0, which the page credits")

    def test_a_page_without_the_engine_download_bar_is_reported(self):
        self.Edit(self.script.with_name("index.html"), "<progress class=\"download\" hidden></progress>", "")
        self.assertReported("does not start with the status")

    def test_a_stale_panel_line_count_is_reported(self):
        panel = self.script.with_name("panel.css")
        panel.write_text(panel.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        self.assertReported("the gallery says model-viewer's page and panel are")

    def test_a_stale_shared_line_count_is_reported(self):
        shared = self.library / "examples" / "shared" / "page-status.js"
        shared.write_text(shared.read_text(encoding="utf-8") + "\n", encoding="utf-8")
        self.assertReported("the gallery says the shared page code is")


if __name__ == "__main__":
    unittest.main()
