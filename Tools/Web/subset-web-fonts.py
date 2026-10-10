#!/usr/bin/env python3
"""Regenerate the web build's cut-down copies of the editor UI fonts.

The shipped Roboto faces carry ~2770 codepoints each — Cyrillic, Greek,
Vietnamese, the full Latin Extended range — and ten of them are 1.9 MB of the
web build's 4.4 MB compressed asset payload, more than any other group. The
editor's own chrome uses ASCII plus about sixty symbols, so the web package
ships subsets instead: same filenames, same directory at runtime, ~440 KB.

Desktop keeps the full faces. It has no size pressure, and a subset is a real
trade: a glyph outside the ranges below renders as tofu. That is the accepted
cost on web for editor chrome, but it also applies to PROJECT text — an asset
or entity named in a script the subset does not cover will not render on web.
Widen the ranges here if that becomes a real complaint; the durable fix is
loading a full face on demand when a glyph misses, which nothing does yet.

Run after changing Assets/Fonts, and commit the result:

    python3 Tools/Web/subset-web-fonts.py            # needs fonttools
"""
import pathlib
import subprocess
import sys

kRepoRoot = pathlib.Path(__file__).resolve().parents[2]
kSourceDir = kRepoRoot / "Apps" / "Editor" / "Assets" / "Fonts"
kOutputDir = kRepoRoot / "Apps" / "Editor" / "WebFonts"

# Every script Roboto actually carries — Latin including Extended-B and the
# Vietnamese range, Greek, Cyrillic — plus the symbol blocks the editor draws
# from. The editor's own chrome needs far less: a scan of the editor and UI
# sources turns up 65 distinct non-ASCII codepoints, all inside Latin-1, Greek
# and the symbol blocks. The scripts are here for PROJECT text, which is named
# by users and is the one thing these ranges cannot be derived from; carrying
# them costs about 44 KB per face and removes the whole class of tofu that a
# chrome-derived subset would have shipped. Emoji stay out on purpose: they
# come from the colour atlas, not from Roboto.
kUnicodeRanges = ",".join((
    "U+0020-007E",   # ASCII
    "U+00A0-00FF",   # Latin-1 Supplement
    "U+0100-017F",   # Latin Extended-A
    "U+0180-024F",   # Latin Extended-B
    "U+1E00-1EFF",   # Latin Extended Additional (Vietnamese)
    "U+0400-04FF",   # Cyrillic
    "U+0500-052F",   # Cyrillic Supplement
    "U+0300-036F",   # combining marks: macOS hands out filenames in NFD, so an
                     # asset named "Cafe\u0301" needs the mark to render at all
    "U+0192",        # florin
    "U+02C6-02DC",   # spacing modifiers
    "U+0370-03FF",   # Greek
    "U+2000-206F",   # General Punctuation
    "U+2070-209F",   # super/subscripts
    "U+20A0-20BF",   # currency
    "U+2100-214F",   # letterlike
    "U+2150-218F",   # number forms (fractions, Roman numerals)
    "U+2190-21FF",   # arrows
    "U+2200-22FF",   # maths operators
    "U+2300-23FF",   # miscellaneous technical
    "U+25A0-25FF",   # geometric shapes
    "U+2500-257F",   # box drawing
    "U+FEFF",        # BOM
    "U+FFFD",        # replacement character
))


def main() -> int:
    if not kSourceDir.is_dir():
        print(f"no font source directory at {kSourceDir}", file=sys.stderr)
        return 1

    kOutputDir.mkdir(parents=True, exist_ok=True)
    faces = sorted(kSourceDir.glob("*.ttf"))
    if not faces:
        print(f"no .ttf faces in {kSourceDir}", file=sys.stderr)
        return 1

    before = after = 0
    for face in faces:
        out = kOutputDir / face.name
        subprocess.run(
            ["pyftsubset", str(face),
             f"--unicodes={kUnicodeRanges}",
             # Kerning and the standard ligature/contextual set; the rest of the
             # layout tables are shaping the editor never asks for.
             "--layout-features=kern,liga,calt",
             # Glyph names, for debuggers. The renderer goes by index.
             "--drop-tables+=post",
             f"--output-file={out}"],
            check=True)
        before += face.stat().st_size
        after += out.stat().st_size
        print(f"  {face.name:<28} {face.stat().st_size/1024:>7.0f} KB ->"
              f" {out.stat().st_size/1024:>6.0f} KB")

    print(f"\n{len(faces)} faces: {before/1024:.0f} KB -> {after/1024:.0f} KB "
          f"({100 * (before - after) / before:.0f}% smaller) in {kOutputDir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
