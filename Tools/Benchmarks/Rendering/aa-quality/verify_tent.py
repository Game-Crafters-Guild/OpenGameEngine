#!/usr/bin/env python3
"""Compare live GPU SSAA output to independent CPU tent convolution in HDR."""
import argparse
import json
from pathlib import Path
import numpy as np
from capture import Editor


from verify_area import read

def integrate_axis(values, count, axis):
    # Independent point-sample convolution, without GPU bilinear pair packing.
    a = np.moveaxis(values, axis, 0)
    radius = max(len(a) / count, 1.0)
    result = []
    for i in range(count):
        center = (i + 0.5) * len(a) / count
        indices = np.arange(int(np.floor(center-radius))-1,
                            int(np.ceil(center+radius))+1)
        weights = np.maximum(1 - np.abs(indices+0.5-center)/radius, 0)
        samples = a[np.clip(indices, 0, len(a)-1)]
        result.append(np.tensordot(weights/weights.sum(), samples, axes=(0,0)))
    return np.moveaxis(np.asarray(result), 0, axis)


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
