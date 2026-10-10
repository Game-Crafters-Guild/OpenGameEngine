#!/usr/bin/env python3
"""Assert a built opengine-core.gepak holds what engine_pack.py's docstring says it holds.

Every file in the pack must match a pattern of the docstring's layout, and every pattern
marked `!` must match at least one file: a pack that drops the render pipeline, an engine
shader directory, the cooked packages, the font or the variant cache fails here instead of as
an empty canvas in a browser. Every built-in package the pack's tables name
(builtin_shader_cook.kPackages and kLibraryPackages) must be in it, so a pass the library's
pipeline runs never finds its program missing at runtime.

    python3 Tools/Web/engine_pack.py --out C:/scratch/opengine-core.gepak --build-tree build/vs2026-x64-local
    python3 Tools/Web/test_engine_pack.py --pack C:/scratch/opengine-core.gepak

Exit 0 = the pack matches its layout; 1 = it does not.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builtin_shader_cook  # noqa: E402
import engine_pack  # noqa: E402
import gepak  # noqa: E402


def PatternRegex(pattern: str) -> re.Pattern[str]:
    parts = []
    for token in re.split(r"(\*\*/|\*\*|\*)", pattern):
        if token == "**/":
            parts.append(r"(?:.*/)?")
        elif token == "**":
            parts.append(r".*")
        elif token == "*":
            parts.append(r"[^/]*")
        else:
            parts.append(re.escape(token))
    return re.compile("".join(parts) + r"\Z")


def CheckPack(paths: list[str], layout: list[tuple[str, bool]]) -> list[str]:
    """The ways `paths` departs from `layout`, one message each; empty when it matches."""
    regexes = [(pattern, required, PatternRegex(pattern)) for pattern, required in layout]
    problems = [f"{path} matches no layout pattern" for path in paths
                if not any(regex.match(path) for _, _, regex in regexes)]
    problems += [f"no file matches the required pattern {pattern}" for pattern, required, regex in regexes
                 if required and not any(regex.match(path) for path in paths)]
    return problems


def MissingPackages(paths: list[str]) -> list[str]:
    """The built-in packages the pack's tables name that the pack does not hold, one message each."""
    present = set(paths)
    names = {**builtin_shader_cook.kPackages, **builtin_shader_cook.kLibraryPackages}
    return [f"the table names {name} but the pack has no Assets/Shaders/{name}.shaderpkg"
            for name in sorted(names) if f"Assets/Shaders/{name}.shaderpkg" not in present]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pack", type=Path, required=True, help="a built opengine-core.gepak")
    args = parser.parse_args()

    layout = engine_pack.Layout()
    if len(layout) < 10 or not any(required for _, required in layout):
        print(f"FAIL: engine_pack.py's docstring layout did not parse ({len(layout)} patterns)")
        return 1
    paths = sorted(entry.Path for entry in gepak.Unpack(args.pack.read_bytes()))
    problems = CheckPack(paths, layout) + MissingPackages(paths)
    for problem in problems:
        print(f"FAIL: {problem}")
    print(f"test_engine_pack: {len(paths)} files against {len(layout)} patterns, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
