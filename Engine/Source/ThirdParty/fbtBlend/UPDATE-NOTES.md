# fbtBlend (Vendored)

Vendored copy of the fbtBlend-Header-Only library by Flix01.

## Upstream

- Repository: https://github.com/Flix01/fbtBlend-Header-Only
- License: Zlib (see `LICENSE` next to this file)
- Commit pinned: `ddb0d3616f4f5ee7b45f62647b269f56f888816d`
- Commit date: 2026-03-19
- Commit message: "Added Blender 5.1 support in folder 501."

## Files vendored

| File | Source path in upstream | Bytes |
|---|---|---|
| `fbtBlend.h` | `405_LTS/fbtBlend.h` (with the local modifications below) | 548 977 upstream |
| `Blender.h`  | `405_LTS/Blender.h`  | 221 515 |
| `LICENSE`    | `LICENSE`            |     900 |
| `fbtBlend.cpp` | (added by us — implementation shim) | ~2.6k |

## Blender version coverage

fbtBlend's `405_LTS/` folder targets Blender 4.0.x through 4.5 LTS.
This is the right tier for our current sample bundles (Blender 4.0.0,
4.0.2). It uses the OLD 12-byte `.blend` header format (`'BLENDER-v405'`
style); the upstream `501/` folder (Blender 5.x) introduced an
INCOMPATIBLE 17-byte header format that rejects every Blender 4.x file
as "Unknown header format version". A binary that needs both Blender
4.x and 5.x files would have to compile two TUs (one per folder) and
dispatch on the file header — captured as a Phase B5+ follow-up; not
needed for the user's three sample bundles.

Once the user provides a Blender 5.x sample, switch to a dual-TU build
(or to the next upstream commit that unifies both header readers).
**Currently-vendored coverage: Blender 2.79 (via SDNA fallback) to
4.5 LTS. Blender 5.0 / 5.1 files are NOT supported.**

## Compression dependencies

- **zlib** (`FBT_USE_GZ_FILE=1`) is required for `.blend` files saved by
  Blender < 3.0 (PM_COMPRESSED, gzip).
- **zstd** (`FBT_USE_ZSTD_FILE=1`) is required for `.blend` files saved
  by Blender >= 3.0 (zstd).

Both libraries are present transitively in our vcpkg build (zlib via
`harfbuzz`/`curl`/`ktx`; zstd via `curl`/`ffmpeg`). `Engine/CMakeLists.txt`
detects them via `find_package(ZLIB)` / `find_package(zstd CONFIG)`,
sets `GE_FBTBLEND_HAVE_ZLIB` / `GE_FBTBLEND_HAVE_ZSTD` accordingly, and
links what is found. Uncompressed `.blend` files always parse.

## Bumping to a newer commit

1. Identify a new commit on the upstream `master` branch.
2. Replace `fbtBlend.h` and `Blender.h` from the latest version folder.
   Keep the file names the same.
3. Re-apply the changes under "Local modifications" below, or drop the
   ones upstream has fixed on the same paths.
4. Replace `LICENSE` only if upstream changed it (rare; pure Zlib).
5. Update the commit hash + date + message above.
6. Build `Engine` target and run `BlendSmokeParseTests` (Phase B1) to
   confirm the bump still parses our 3 sample bundles and still rejects
   the hostile inputs cleanly.

## Local modifications

`fbtBlend.h` differs from the pinned upstream commit in `fbtMemoryStream`
only:

- Every release of `m_buffer` outside a reallocation goes through
  `fbtMemoryStream::release()`, which `delete[]`s it, nulls it and zeroes
  size, capacity and position. Upstream left `m_buffer` dangling after a
  failed zstd decode and the gzip fallback freed it again, so any
  compressed `.blend` that was not valid zstd (a gzip legacy file, a
  truncated file, junk) corrupted the heap (0xC0000374). Upstream also
  `free()`d `new[]` memory when `inflateInit2` failed, and took the
  caller's input as its own buffer on empty zstd input.
- The memory-buffer `open(const void*, FBTsize, StreamMode, bool)`
  releases the previous buffer before loading a new one.
- `gzipInflate` sets `m_size` to the inflated byte count before it grows
  the buffer (upstream's growth copied zero bytes and lost everything
  inflated so far), and an inflate error is a failed decode instead of a
  success with partial output.
- `gzipInflate` grows its buffer geometrically (by half the capacity plus
  half the input) instead of by a fixed half of the input, so inflating
  a highly compressed file copies linearly, not quadratically.
- The copy constructor and copy assignment are deleted: the stream owns
  `m_buffer`, and upstream's implicit copies would free it twice.

`BlendSmokeParseTests` feeds junk, a truncated zstd blend and a gzip
legacy blend through the in-memory parse the model loader uses.

## Why vendored, not vcpkg

fbtBlend is two header files plus a 50-byte `LICENSE` and is not on
vcpkg. Vendoring (a) pins a commit hash for DNA-version
reproducibility, (b) follows existing engine practice for
`stb_image`, `tinygltf`, `ufbx`, and (c) avoids a vcpkg port
maintenance burden for a library that is still effectively a single
maintainer's project.
