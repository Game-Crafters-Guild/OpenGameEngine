#!/usr/bin/env python3
"""Compare the historical AA before/after experiment and create a sampled animation.

This is a baseline-specific benchmark, not a general automated correctness gate.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
from analyze import analyze


def compare(before, after):
    analyze(before)
    analyze(after)
    a = {r["mode"]: r for r in json.loads((before / "summary.json").read_text())}
    b = {r["mode"]: r for r in json.loads((after / "summary.json").read_text())}
    old_conditions = json.loads((before / "conditions.json").read_text())
    new_conditions = json.loads((after / "conditions.json").read_text())
    # Preferences serialize additional UI defaults on first shutdown, and EV
    # round-trips through float32. Compare controlled rendering fields; the
    # byte-identical Off control below catches effective view/image changes.
    for name in ("postFxAutoExposureEnabled", "postFxFixedExposureEv",
                 "postFxExposureCompensation", "postFxBloomEnabled",
                 "postFxCasEnabled", "postFxColorFilterEnabled", "postFxCrtEnabled"):
        key = "sceneView." + name
        assert key in old_conditions and key in new_conditions, key
        assert np.isclose(old_conditions[key], new_conditions[key], rtol=0, atol=1e-5), key
    assert set(a) == set(b), "Incomplete comparison matrix"
    for mode in a:
        assert (a[mode]["width"], a[mode]["height"]) == (b[mode]["width"], b[mode]["height"])
    off_before = np.asarray(Image.open(before / "off/static-00.png"))
    off_after = np.asarray(Image.open(after / "off/static-00.png"))
    assert np.array_equal(off_before, off_after), "Single-sample control changed"
    for mode in ["msaa2", "msaa4"]:
        assert a[mode]["redDiagnosticPixels"] > 0
        assert b[mode]["redDiagnosticPixels"] == 0, "MSAA extrapolation artifacts remain"
    for mode in ["taa", "taau"]:
        assert b[mode]["staticEdgeStdCodes"] < a[mode]["staticEdgeStdCodes"] * .2, "Insufficient stability improvement"
    # Explicitly slowed, sampled playback. Capture frame indices remain in the
    # manifests; this GIF must never be described as consecutive-frame video.
    frames = []
    crops = (340, 250, 1160, 640)
    for i in range(16):
        canvas = Image.new("RGB", (1640, 430), "#15181f")
        draw = ImageDraw.Draw(canvas)
        for x, root, label in [(0, before, "Before"), (820, after, "After")]:
            im = Image.open(root / "taa" / f"static-{i:02}.png").convert("RGB").crop(crops)
            canvas.paste(im, (x, 40))
            draw.text((x+12, 12), label + " — sampled static TAA, slowed playback", fill="white")
        frames.append(canvas)
    frames[0].save(after / "taa-stability.gif", save_all=True, append_images=frames[1:], duration=180, loop=0)
    if (after / "taa/reveal-07.png").exists():
        canvas = Image.new("RGB", (1200, 600), "#191919")
        draw = ImageDraw.Draw(canvas)
        samples = [("taa", "static-00"), ("taa", "occlusion-covered"),
                   ("taa", "reveal-00"), ("taa", "reveal-07"),
                   ("taau", "reveal-00"), ("taau", "reveal-07")]
        for i, (mode, name) in enumerate(samples):
            im = Image.open(after / mode / (name + ".png")).crop((350, 470, 700, 640)).resize((400, 240))
            x, y = (i % 3)*400, (i // 3)*300
            canvas.paste(im, (x, y+30))
            draw.text((x+8, y+8), mode + " " + name, fill="white")
        canvas.save(after / "reveal-comparison.png")
    print("PASS: historical benchmark control parity, MSAA edge interpolation, TAA/TAAU stability")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()
    compare(args.before, args.after)
