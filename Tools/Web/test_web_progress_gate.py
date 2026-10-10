#!/usr/bin/env python3
"""The package gate accepts both download progress states and rejects a hidden or stalled bar."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import web_package_gate as gate


def Sample(value: int = 10, *, unknown: bool = False, shown: bool = True,
           sweep: str = "-20% 0px, 0px 0px") -> dict:
    return {"status": "Loading SciFi helmet..." if unknown else f"Loading SciFi helmet {value}%",
            "shown": shown, "value": 0 if unknown else value, "max": 100,
            "position": -1 if unknown else value / 100, "sweep": sweep}


class SampleConnection:
    def __init__(self, *samples: dict):
        self.samples = list(samples)
        self.index = 0

    def Pump(self, seconds: float) -> None:
        self.index = min(self.index + 1, len(self.samples) - 1)


class ProgressSamplesTests(unittest.TestCase):
    def Failures(self, first: dict, second: dict) -> list[str]:
        connection = SampleConnection(first, second)
        with patch.object(gate, "Evaluate", side_effect=lambda cdp, expression: cdp.samples[cdp.index]):
            return gate.ProgressSamples(connection, "SciFi helmet", 0)["failures"]

    def test_known_size_bar_advances(self):
        self.assertEqual(self.Failures(Sample(10), Sample(30)), [])

    def test_unknown_size_bar_sweeps(self):
        self.assertEqual(self.Failures(Sample(unknown=True), Sample(unknown=True, sweep="120% 0px, 0px 0px")), [])

    def test_known_size_stall_fails(self):
        self.assertTrue(self.Failures(Sample(10), Sample(10)))

    def test_unknown_size_stall_fails(self):
        self.assertTrue(self.Failures(Sample(unknown=True), Sample(unknown=True)))

    def test_hidden_bar_fails_for_either_size(self):
        for unknown in (False, True):
            with self.subTest(unknown=unknown):
                self.assertTrue(self.Failures(Sample(10, unknown=unknown, shown=False),
                                             Sample(30, unknown=unknown, shown=False, sweep="120% 0px, 0px 0px")))

    def test_page_before_its_first_progress_report_is_not_a_download_sample(self):
        # The page's own markup: the status names the download, the bar is still hidden.
        before = Sample(unknown=True, shown=False)
        connection = SampleConnection(before, Sample(10), Sample(30))
        with patch.object(gate, "Evaluate", side_effect=lambda cdp, expression: cdp.samples[cdp.index]):
            self.assertEqual(gate.ProgressSamples(connection, "SciFi helmet", 5)["failures"], [])

    def test_completion_is_not_a_download_sample(self):
        first, second = Sample(100), Sample(100)
        first["status"] = second["status"] = "Showing SciFi helmet"
        self.assertTrue(self.Failures(first, second))

    def test_size_becoming_known_is_progress(self):
        self.assertEqual(self.Failures(Sample(unknown=True), Sample(30)), [])

    def test_hidden_compression_header_can_make_size_unknown(self):
        self.assertEqual(self.Failures(Sample(10), Sample(unknown=True)), [])

    def test_percentage_text_requires_a_determinate_bar(self):
        first, second = Sample(10), Sample(30)
        first["position"] = second["position"] = -1
        self.assertTrue(self.Failures(first, second))

    def test_another_models_status_is_rejected(self):
        first, second = Sample(10), Sample(30)
        first["status"] = "Loading Water bottle 10%"
        self.assertTrue(self.Failures(first, second))


class GateMirrorsPageTests(unittest.TestCase):
    """The gate's kModelFramed measures with copies of the page's framing constants; each must
    equal the page's (examples/shared/page-status.js)."""

    def test_the_framing_constants_equal_the_page_s(self):
        page = (Path(__file__).resolve().parents[2] / "Apps" / "WebLibrary" / "examples" / "shared" /
                "page-status.js").read_text(encoding="utf-8")
        for name, value in (("kHullPoints", gate.kHullPoints), ("kFitMargin", gate.kFitMargin),
                            ("kOverlayGap", gate.kOverlayGap)):
            with self.subTest(name=name):
                match = re.search(rf"^const {name} = ([0-9.]+);", page, re.MULTILINE)
                self.assertIsNotNone(match, f"page-status.js has no {name}")
                self.assertEqual(float(match.group(1)), float(value))

    def test_the_standard_size_and_the_starting_views_equal_the_page_s(self):
        examples = Path(__file__).resolve().parents[2] / "Apps" / "WebLibrary" / "examples"
        page = (examples / "shared" / "page-status.js").read_text(encoding="utf-8")
        self.assertEqual(float(re.search(r"^const kModelReach = ([0-9.]+);", page, re.MULTILINE).group(1)), gate.kModelReach)
        table = (examples / "model-viewer" / "panel.js").read_text(encoding="utf-8")
        entries = re.findall(r"\['([^']+)'[^\n]*\{ pitch: (-?[0-9.]+), yaw: (-?[0-9.]+)[^}]*\}\]", table)
        self.assertEqual({name: float(pitch) for name, pitch, _ in entries}, gate.kModelPitch)
        self.assertEqual({name: float(yaw) for name, _, yaw in entries}, gate.kModelYaw)


@unittest.skipUnless(shutil.which("node"), "node is not on PATH")
class BoxInHullTests(unittest.TestCase):
    """kBoxInHull, the fit check's bound on the model's box: inside the hull with the inscribed
    polygon's slack, and not beside it."""

    def Inside(self, box: dict) -> bool:
        hull = {"left": 100, "right": 300, "top": 50, "bottom": 250}
        script = f"console.log(JSON.stringify({gate.kBoxInHull}({json.dumps(box)}, {json.dumps(hull)})))"
        return json.loads(subprocess.run(["node", "-e", script], capture_output=True, text=True, check=True).stdout)

    def test_a_box_inside_the_hull_or_within_its_slack_passes_and_one_beside_it_fails(self):
        self.assertTrue(self.Inside({"left": 150, "right": 250, "top": 60, "bottom": 240}))
        self.assertTrue(self.Inside({"left": 99, "right": 200, "top": 60, "bottom": 240}))   # one pixel out: the slack
        self.assertFalse(self.Inside({"left": 60, "right": 160, "top": 60, "bottom": 240}))  # 40 px beside the hull


class TimelineConnection:
    """A page whose status follows `statuses`, one every `step` seconds, on a clock that only the
    gate's own pumping advances."""
    def __init__(self, statuses: list[str], step: float):
        self.statuses, self.step, self.now = statuses, step, 0.0

    def Pump(self, seconds: float) -> None:
        self.now += seconds

    def Sample(self) -> dict:
        status = self.statuses[min(int(self.now / self.step), len(self.statuses) - 1)]
        return {"status": status, "shown": True, "position": -1, "value": 0, "max": 1}


class StartingAfterDownloadTests(unittest.TestCase):
    def Failures(self, statuses: list[str], step: float, seconds: float) -> list[str]:
        connection = TimelineConnection(statuses, step)
        with (patch.object(gate, "Evaluate", side_effect=lambda cdp, expression: cdp.Sample()),
              patch.object(gate.time, "time", side_effect=lambda: connection.now)):
            return gate.StartingAfterDownload(connection, "Water bottle", seconds)()[1]

    def test_a_download_longer_than_the_wait_passes_while_it_advances(self):
        counting = [f"Loading the engine {percent}%" for percent in range(100)]
        self.assertEqual(self.Failures([*counting, gate.kStarting], step=1.0, seconds=5.0), [])

    def test_a_stalled_download_fails(self):
        self.assertTrue(self.Failures(["Loading the engine 40%"] * 20 + [gate.kStarting], step=1.0, seconds=5.0))


if __name__ == "__main__":
    unittest.main()
