#!/usr/bin/env python3
"""Stage the web library's examples against the engine modules a build produced, for the
browser gate (Tools/Web/browser_gate.py).

    python3 Tools/Web/stage_web_library.py --out C:/scratch/weblib-stage \\
        --st-bin build/wasm-release-singlethread/bin --mt-bin build/wasm-release/bin \\
        --build-tree build/vs2026-x64-local

    python3 Tools/Web/browser_gate.py --root C:/scratch/weblib-stage \\
        --page examples/model-viewer/index.html --screenshot mt.png

The staged tree mirrors Apps/WebLibrary, so each example finds the facade where its import
map points (ts/dist/src) and the facade finds the engine beside itself:

    examples/                  the examples, as authored
    examples/model-viewer/st.*  the model-viewer page with threads: 'single'; a gate serves
                               every page cross-origin isolated, where 'auto' picks 'mt'
    examples/model-viewer/dispose.*  the gate's dispose check (Tools/Web/web-library-gate/
                               dispose.js), ?threads=single|auto; browser_gate.py
                               --expect "dispose-check: PASS" reads its verdict
    examples/model-viewer/switch.*   the gate's model-switch viewer (web-library-gate/
                               switch.js), driven by browser_gate.py --step
                               "geSwitchTo('<model>')"
    ts/dist/src/               the facade (built with Apps/WebLibrary/ts/test/run-tests.mjs)
                               plus opengine-core-binding.js, opengine-core.{st,mt}.{js,wasm}
                               and opengine-core.gepak

opengine-core.gepak is built by Tools/Web/engine_pack.py, whose docstring defines it.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import engine_pack  # noqa: E402

kRepoRoot = Path(__file__).resolve().parents[2]
kLibraryDir = kRepoRoot / "Apps" / "WebLibrary"
kGatePages = Path(__file__).resolve().parent / "web-library-gate"
kEnginePackName = "opengine-core.gepak"


class StageError(Exception):
    """A stage that cannot produce a servable tree."""


def CopyCore(binDir: Path, build: str, outDir: Path) -> None:
    for suffix in ("js", "wasm"):
        source = binDir / f"opengine-core.{build}.{suffix}"
        if not source.is_file():
            raise StageError(f"{source} is missing: build the WebLibrary target in that tree.")
        shutil.copy2(source, outDir / source.name)


def StageSingleThreadedPage(examples: Path) -> None:
    page = examples / "model-viewer"
    script = (page / "main.js").read_text(encoding="utf-8")
    if "threads: 'auto'" not in script:
        raise StageError("model-viewer/main.js no longer passes threads: 'auto'; update the stager.")
    (page / "st.js").write_text(script.replace("threads: 'auto'", "threads: 'single'"), encoding="utf-8")
    html = (page / "index.html").read_text(encoding="utf-8")
    (page / "st.html").write_text(html.replace('src="main.js"', 'src="st.js"'), encoding="utf-8")


def StageGatePages(examples: Path) -> None:
    page = examples / "model-viewer"
    html = (page / "index.html").read_text(encoding="utf-8")
    for name in ("dispose", "switch"):
        shutil.copy2(kGatePages / f"{name}.js", page / f"{name}.js")
        (page / f"{name}.html").write_text(html.replace('src="main.js"', f'src="{name}.js"'), encoding="utf-8")


def Stage(args: argparse.Namespace) -> None:
    facade = kLibraryDir / "ts" / "dist" / "src"
    if not (facade / "index.js").is_file():
        raise StageError(f"{facade} is missing: build the facade with node Apps/WebLibrary/ts/test/run-tests.mjs.")
    if args.out.exists():
        shutil.rmtree(args.out)
    shutil.copytree(kLibraryDir / "examples", args.out / "examples")
    StageSingleThreadedPage(args.out / "examples")
    StageGatePages(args.out / "examples")
    core = args.out / "ts" / "dist" / "src"
    shutil.copytree(facade, core)
    shutil.copy2(kLibraryDir / "opengine-core-binding.js", core / "opengine-core-binding.js")
    CopyCore(args.st_bin, "st", core)
    CopyCore(args.mt_bin, "mt", core)
    paths = engine_pack.build_engine_pack(core / kEnginePackName, args.build_tree)
    print(f"stage: {kEnginePackName}: {len(paths)} files")
    print(f"stage: {args.out}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True, help="directory to create")
    parser.add_argument("--st-bin", type=Path, required=True, help="bin directory of the single-threaded build")
    parser.add_argument("--mt-bin", type=Path, required=True, help="bin directory of the threaded build")
    parser.add_argument("--build-tree", type=Path, required=True,
                        help="a desktop build with CompileShaderPkgs and MaterialVariantCook built")
    args = parser.parse_args()
    try:
        Stage(args)
    except (StageError, engine_pack.EnginePackError, engine_pack.export_web_player.ExportError) as error:
        print(f"stage: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
