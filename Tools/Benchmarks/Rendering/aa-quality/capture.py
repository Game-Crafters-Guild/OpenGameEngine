#!/usr/bin/env python3
"""Capture AA comparisons from an isolated editor's documented debug protocol.

Requires Pillow and NumPy only for analysis (see analyze.py). Capture itself uses
the standard library. Samples are timestamped, NOT consecutive frame captures.
"""
import argparse
import base64
import json
import socket
import time
from pathlib import Path


class Editor:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=60)
        self.stream = self.socket.makefile("rb")
        self.sequence = 0

    def call(self, method, **params):
        self.sequence += 1
        self.socket.sendall((json.dumps({"id": str(self.sequence), "method": method,
                                       "params": params}) + "\n").encode())
        result = json.loads(self.stream.readline())
        if not result.get("ok"):
            raise RuntimeError(result)
        result = result.get("result", {})
        if isinstance(result, dict) and result.get("error"):
            raise RuntimeError(result)
        return result

    def ready(self):
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            state = self.call("get_editor_state")
            if state.get("sceneLoadError") or state.get("sceneDegraded"):
                raise RuntimeError(state)
            if not state.get("pendingMaterialTextureBinds"):
                return state
            time.sleep(.25)
        raise RuntimeError("Materials did not settle")

    def warm(self):
        # Texture readiness alone does not include asynchronous shader variants.
        deadline = time.monotonic() + 120
        previous, stable_since = None, time.monotonic()
        while time.monotonic() < deadline:
            self.ready()
            counts = self.call("get_render_stats").get("variantCacheCounts")
            if counts != previous:
                previous, stable_since = counts, time.monotonic()
            if time.monotonic() - stable_since > 3:
                return
            time.sleep(.25)
        raise RuntimeError("Shader variants did not settle")

    def shot(self, path):
        result = self.call("take_screenshot", target="viewport", dither=False, deband=False)
        if result.get("method") != "rendergraph" or result.get("pendingMaterialTextureBinds"):
            raise RuntimeError("Capture was not a ready rendergraph viewport")
        path.write_bytes(base64.b64decode(result.pop("pngBase64")))
        result["captureTime"] = time.monotonic()
        result["image"] = path.name
        return result


MODES = {"off": ("off", 1, 1), "taa": ("taa", 1, 1),
         "taau": ("taa", .67, 1), "msaa2": ("msaa", 1, 2),
         "msaa4": ("msaa", 1, 4), "msaa8": ("msaa", 1, 8),
         "ssaa125": ("ssaa", 1.25, 1), "ssaa150": ("ssaa", 1.5, 1),
         "ssaa200": ("ssaa", 2, 1)}


def capture(args):
    editor = Editor(args.port)
    state = editor.ready()
    if Path(state["session"]["worktreePath"]).resolve() != args.worktree.resolve():
        raise RuntimeError("Wrong worktree/editor; refusing to change its scene")
    project = args.worktree / "Artifacts/AAQuality/Project"
    editor.call("open_scene", path=str(project / "Assets/Scenes/AAQuality.scene"))
    editor.ready()
    editor.call("focus_view", view="scene")
    editor.call("set_gizmos_visibility", all=False)
    editor.call("set_camera", position=[0, 0, -18], yawDeg=90, pitchDeg=0)
    editor.call("get_gpu_profiler", enabled=True)
    entities = editor.call("get_scene_hierarchy")["entities"]
    occluder = next(e["id"] for e in entities if e["name"] == "Occluder")
    args.output.mkdir(parents=True, exist_ok=True)
    preferences = args.worktree / "Artifacts/AAQuality/EditorData/Preferences.json"
    if preferences.exists():
        settings = json.loads(preferences.read_text())
        (args.output / "conditions.json").write_text(json.dumps(
            {k:v for k,v in settings.items() if k.startswith("sceneView.")}, indent=2))
    for label in args.modes.split(","):
        mode, scale, samples = MODES[label]
        folder = args.output / label
        folder.mkdir(exist_ok=True)
        editor.call("set_aa_mode", mode=mode)
        editor.call("set_msaa", samples=samples)
        editor.call("set_taa_render_scale", scale=scale)
        editor.call("set_camera", position=[0, 0, -18], yawDeg=90, pitchDeg=0)
        editor.warm()
        manifest = {"mode": label, "requested": {"mode": mode, "scale": scale, "samples": samples},
                    "state": editor.ready(), "aa": editor.call("get_msaa"),
                    "camera": editor.call("get_camera"),
                    "resources": editor.call("get_render_graph_resources"),
                    "static": [], "pan": [], "timings": []}
        manifest["validation"] = editor.call("get_render_graph_validation")
        assert manifest["validation"]["errorCount"] == 0, manifest["validation"]
        resources = manifest["resources"]["resources"]
        scene_color = next(r for r in resources if r["name"].endswith(".SceneColor") and r["used"])
        view_color = next(r for r in resources if r["name"].endswith("SceneView.Color"))
        actual = "off" if mode == "ssaa" else mode
        assert manifest["aa"]["aaMode"] == actual, manifest["aa"]
        if mode == "msaa":
            assert view_color["used"] and view_color["sampleCount"] == manifest["aa"]["samples"] > 1
        else:
            assert abs(scene_color["width"] / view_color["width"] - scale) < .003
            assert abs(scene_color["height"] / view_color["height"] - scale) < .003
        if mode == "taa":
            assert any(r["name"].startswith("TAA.") and "History" in r["name"] and r["used"] for r in resources)
        for i in range(args.samples):
            manifest["static"].append(editor.shot(folder / f"static-{i:02}.png"))
        # Sample profiling without readbacks in flight. Metal spans are NOT
        # additive pass timings; preserve the source's timing semantics.
        for i in range(20):
            time.sleep(.05)
            manifest["timings"].append(editor.call("get_gpu_profiler"))
        # Repeatable small camera translations. There may be multiple rendered
        # frames between commands: frame indices are recorded to make this explicit.
        for i in range(args.pan_samples):
            x = (i+1) * .0075
            editor.call("set_camera", position=[x, 0, -18])
            shot = editor.shot(folder / f"pan-{i:02}.png")
            shot["cameraX"] = x
            manifest["pan"].append(shot)
        if args.occlusion:
            editor.call("set_camera", position=[0, 0, -18], yawDeg=90, pitchDeg=0)
            editor.call("set_component", entityId=occluder, component="Transform",
                        values={"position": {"x": -4, "y": -.7, "z": -.6}})
            editor.warm()
            manifest["occlusionCovered"] = editor.shot(folder / "occlusion-covered.png")
            editor.call("set_component", entityId=occluder, component="Transform",
                        values={"position": {"x": -9, "y": -.7, "z": -.6}})
            manifest["reveal"] = [editor.shot(folder / f"reveal-{i:02}.png") for i in range(8)]
        (folder / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(label, manifest["aa"], flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=10019)
    parser.add_argument("--worktree", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--modes", default=",".join(MODES))
    parser.add_argument("--samples", type=int, default=16)
    parser.add_argument("--pan-samples", type=int, default=16)
    parser.add_argument("--occlusion", action="store_true")
    capture(parser.parse_args())
