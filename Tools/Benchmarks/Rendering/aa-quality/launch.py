#!/usr/bin/env python3
"""Launch an AA lab editor with isolated assets and preferences.

Looks for a staged Editor under build/<preset>/bin/<config>/ on macOS,
Windows, and Linux. Pass --exe when that default is not present.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys


def default_preferences_source() -> Path:
    home = Path.home()
    if sys.platform == "darwin":
        return home / "Library/Application Support/GameEngine/Editor/Preferences.json"
    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA", str(home / "AppData/Roaming"))
        return Path(appdata) / "GameEngine/Editor/Preferences.json"
    xdg = os.environ.get("XDG_DATA_HOME", str(home / ".local/share"))
    return Path(xdg) / "GameEngine/Editor/Preferences.json"


def default_editor(root: Path) -> Path:
    env = os.environ.get("GE_EDITOR")
    if env:
        return Path(env)
    candidates = (
        root / "build/macos-arm64-ninja/bin/Release/Apps/Editor/Editor.app/Contents/MacOS/Editor",
        root / "build/macos-arm64-local/bin/Release/Apps/Editor/Editor.app/Contents/MacOS/Editor",
        root / "build/vs2026-x64-local/bin/Release/Apps/Editor/Editor.exe",
        root / "build/vs2026-x64-local/bin/DebugFast/Apps/Editor/Editor.exe",
        root / "build/linux-x64-ninja/bin/Release/Apps/Editor/Editor",
    )
    for path in candidates:
        if path.exists():
            return path
    raise SystemExit("No staged Editor found. Pass --exe /path/to/Editor (or GE_EDITOR).")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=10019)
    parser.add_argument("--label", default="aa-quality")
    parser.add_argument("--exe", type=Path, help="Editor executable; overrides GE_EDITOR and the staged defaults")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[4]
    output = root / "Artifacts/AAQuality"
    profile = output / "EditorData"
    profile.mkdir(parents=True, exist_ok=True)
    try:
        connection = socket.create_connection(("127.0.0.1", args.port), timeout=.2)
    except OSError:
        pass
    else:
        connection.close()
        raise SystemExit(f"Port {args.port} is occupied; shut down that test session first")
    preferences = profile / "Preferences.json"
    if not preferences.exists():
        source = default_preferences_source()
        settings = json.loads(source.read_text()) if source.exists() else {}
        # This writes only the isolated profile. Exposure is fixed and recorded;
        # sharpen/bloom/CRT cannot disguise reconstruction artifacts.
        settings.update({"sceneView.postFxAutoExposureEnabled": False,
                         "sceneView.postFxFixedExposureEv": 16.9,
                         "sceneView.postFxExposureCompensation": 0,
                         "sceneView.postFxBloomEnabled": False,
                         "sceneView.postFxCasEnabled": False,
                         "sceneView.postFxColorFilterEnabled": False,
                         "sceneView.postFxCrtEnabled": False})
        preferences.write_text(json.dumps(settings, indent=2) + "\n")
    exe = args.exe.resolve() if args.exe else default_editor(root)
    if not exe.exists():
        raise SystemExit(f"Editor not found: {exe}")
    env = os.environ.copy()
    env.update(GE_EDITOR_USER_DATA_ROOT=str(profile), GE_OUTPUT_DITHER="0", GE_DEBAND="0")
    # Ambient AA overrides would silently defeat the comparison matrix.
    for name in ("GE_AA_MODE", "GE_MSAA_SAMPLES", "GE_TAA_RENDER_SCALE"):
        env.pop(name, None)
    log = output / (args.label + ".log")
    with log.open("w") as stream:
        process = subprocess.Popen([str(exe), "--project", str(output / "Project"),
                                    "--debug-port", str(args.port), "--session-label", args.label],
                                   cwd=exe.parent, env=env, stdout=stream,
                                   stderr=subprocess.STDOUT, start_new_session=True)
    print(json.dumps({"pid": process.pid, "port": args.port, "log": str(log),
                      "profile": str(profile), "exe": str(exe)}))
