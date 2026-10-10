#!/usr/bin/env python3
"""Compare live GPU SSAA output to independent CPU area integration in HDR."""
import argparse
import base64
import json
from pathlib import Path
import numpy as np
from capture import Editor


def read(editor, name):
    resources = editor.call("get_render_graph_resources")["resources"]
    resource = next(r for r in resources if r["name"] == name)
    tiles = []
    for y in range(0, resource["height"], 96):
        r = editor.call("capture_resource", name=name, raw=True, x=0, y=y,
                        w=resource["width"], h=min(96,resource["height"]-y))
        if r["format"] != "R16G16B16A16_FLOAT" or not r["writtenThisFrame"]:
            raise RuntimeError({k:v for k,v in r.items() if k != "rawBase64"})
        tiles.append(np.frombuffer(base64.b64decode(r["rawBase64"]), dtype="<f2").reshape(
            r["height"], r["width"], 4).astype(np.float64))
    return np.concatenate(tiles, axis=0)


def integrate_axis(values, count, axis):
    # Integral of piecewise-constant texel areas; evaluate at destination pixel
    # boundaries, then difference. Independent of GPU bilinear pair packing.
    a = np.moveaxis(values, axis, 0)
    n = len(a)
    boundaries = np.linspace(0, n, count+1)
    indices = np.floor(boundaries).astype(int)
    partial = (boundaries-indices).reshape((-1,) + (1,)*(a.ndim-1))
    summed = np.concatenate([np.zeros_like(a[:1]), np.cumsum(a, axis=0)], axis=0)
    integral = summed[indices] + partial*a[np.minimum(indices, n-1)]
    return np.moveaxis(np.diff(integral, axis=0) / (n/count), 0, axis)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=10019)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    editor = Editor(args.port)
    state = editor.ready()
    worktree = Path(__file__).resolve().parents[4]
    if Path(state["session"]["worktreePath"]).resolve() != worktree:
        raise RuntimeError("Wrong worktree/editor; refusing to change AA settings")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for scale in [1.25, 1.5, 2.0]:
        editor.call("set_aa_mode", mode="ssaa")
        editor.call("set_taa_render_scale", scale=scale)
        editor.call("set_camera", position=[0,0,-18], yawDeg=90, pitchDeg=0)
        editor.warm()
        passes = editor.call("get_render_graph_passes")["passes"]
        resolve = next(p for p in passes if p["name"].endswith(".SpatialUpscale"))
        accesses = editor.call("get_render_graph_pass_detail", name=resolve["name"])["accesses"]
        source = read(editor, next(a["resource"] for a in accesses if not a["write"]))
        output = read(editor, next(a["resource"] for a in accesses if a["write"]))
        expected = integrate_axis(integrate_axis(source, output.shape[1], 1), output.shape[0], 0)
        error = np.abs(output[:,:,:3]-expected[:,:,:3])
        # Relative to full input range: black pixels must not divide by zero.
        peak = max(float(source[:,:,:3].max()), 1e-6)
        row = {"scale": scale, "source": list(source.shape[:2]), "output": list(output.shape[:2]),
               "meanAbsoluteError": float(error.mean()), "maxAbsoluteError": float(error.max()),
               "normalizedMeanError": float(error.mean()/peak),
               "normalizedP99Error": float(np.percentile(error,99)/peak),
               "normalizedMaxError": float(error.max()/peak)}
        rows.append(row)
        print(row, flush=True)
    args.output.write_text(json.dumps(rows, indent=2) + "\n")
