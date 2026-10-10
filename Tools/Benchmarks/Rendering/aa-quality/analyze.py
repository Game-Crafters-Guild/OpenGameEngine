#!/usr/bin/env python3
"""Summarize sampled static stability and retain visual comparisons.

No claim of motion-compensated error is made: pan captures are visual evidence,
not frame-locked sequences. PNG measurements are in display code values.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw


def analyze(root):
    rows, tiles = [], []
    for manifest_path in sorted(root.glob("*/manifest.json")):
        folder = manifest_path.parent
        manifest = json.loads(manifest_path.read_text())
        frames = np.stack([np.asarray(Image.open(folder / x["image"]).convert("RGB"),
                                      dtype=np.float32) for x in manifest["static"]])
        mean = frames.mean(axis=0)
        std = frames.std(axis=0).mean(axis=2)
        # Threshold out flat black background when reporting detail instability.
        luma = mean.mean(axis=2)
        edge = (np.abs(np.gradient(luma, axis=0)) + np.abs(np.gradient(luma, axis=1))) > 8
        red = (mean[:,:,0] > mean[:,:,1] + 30) & (mean[:,:,0] > mean[:,:,2] + 30)
        timings = manifest["timings"]
        spans = [x["resolveStats"]["distinctSpanGpuMs"] for x in timings
                 if x["resolveStats"]["resolvedPasses"] > 0]
        rows.append({"mode": manifest["mode"], "width": frames.shape[2], "height": frames.shape[1],
                     "staticMeanStdCodes": float(std.mean()),
                     "staticEdgeStdCodes": float(std[edge].mean()) if edge.any() else None,
                     "staticP99StdCodes": float(np.percentile(std, 99)),
                     "redDiagnosticPixels": int(red.sum()),
                     "gpuDistinctSpanMedianMs": float(np.median(spans)) if spans else None,
                     "timingSemantics": timings[0]["timingSemantics"],
                     "sampleFrames": [x.get("frameIndex") for x in manifest["static"]]})
        tile = Image.new("RGB", (640, 400), "#101318")
        im = Image.fromarray(np.clip(mean, 0, 255).astype(np.uint8))
        im.thumbnail((640, 365))
        tile.paste(im, ((640-im.width)//2, 32))
        ImageDraw.Draw(tile).text((12, 10), manifest["mode"], fill="white")
        tiles.append(tile)
    if not tiles:
        raise RuntimeError(f"No completed capture manifests in {root}")
    canvas = Image.new("RGB", (640*3, 400*((len(tiles)+2)//3)), "#101318")
    for i, tile in enumerate(tiles):
        canvas.paste(tile, ((i%3)*640, (i//3)*400))
    canvas.save(root / "comparison.png")
    (root / "summary.json").write_text(json.dumps(rows, indent=2) + "\n")
    print(json.dumps(rows, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    analyze(parser.parse_args().root)
