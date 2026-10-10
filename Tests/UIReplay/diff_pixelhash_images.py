import json
import os
import struct
import zlib
from itertools import groupby


def _read_ppm_p6(path: str):
    with open(path, "rb") as f:
        def tok():
            t = b""
            while True:
                c = f.read(1)
                if not c:
                    return None
                if c in b" \t\r\n":
                    continue
                if c == b"#":
                    f.readline()
                    continue
                t = c
                break
            while True:
                c = f.read(1)
                if not c or c in b" \t\r\n":
                    break
                t += c
            return t

        if tok() != b"P6":
            raise RuntimeError(f"not P6 ppm: {path}")
        w = int(tok())
        h = int(tok())
        maxv = int(tok())
        if maxv != 255:
            raise RuntimeError(f"unsupported maxv {maxv}: {path}")
        data = f.read(w * h * 3)
        if len(data) != w * h * 3:
            raise RuntimeError(f"short ppm data: {path}")
        return w, h, data


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(tag)
    crc = zlib.crc32(data, crc) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)


def _write_png_rgb8(path: str, w: int, h: int, rgb: bytes):
    stride = w * 3
    rows = [b"\x00" + rgb[y * stride : (y + 1) * stride] for y in range(h)]  # filter=0
    raw = b"".join(rows)
    comp = zlib.compress(raw, 9)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # RGB8
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(_png_chunk(b"IHDR", ihdr))
        f.write(_png_chunk(b"IDAT", comp))
        f.write(_png_chunk(b"IEND", b""))


def _draw_rect(rgb: bytearray, w: int, h: int, x0: int, y0: int, x1: int, y1: int, color):
    r, g, b = color

    def set_px(x: int, y: int):
        if 0 <= x < w and 0 <= y < h:
            i = (y * w + x) * 3
            rgb[i] = r
            rgb[i + 1] = g
            rgb[i + 2] = b

    for x in range(x0, x1 + 1):
        set_px(x, y0)
        set_px(x, y1)
    for y in range(y0, y1 + 1):
        set_px(x0, y)
        set_px(x1, y)


def _load_hash_map(log_path: str):
    # 8e-7: hashes carry the producing arm/source; frames hashed on different
    # arms or sources (old backbuffer bytes vs RG2 FinalLinear) are not
    # comparable and must be treated as skipped, not mismatched.
    m = {}
    with open(log_path, "r", encoding="utf-8") as f:
        for line in f:
            j = json.loads(line)
            if j.get("kind") == "ui_replay_pixel_hash":
                m[int(j["frame"])] = (
                    j["hash64"],
                    j.get("arm", "old"),
                    j.get("src", "backbuffer"),
                )
    return m


def _first_mismatch_frame(mode0_log: str, mode1_log: str):
    m0 = _load_hash_map(mode0_log)
    m1 = _load_hash_map(mode1_log)
    for fr in sorted(set(m0) & set(m1)):
        h0, arm0, src0 = m0[fr]
        h1, arm1, src1 = m1[fr]
        if (arm0, src0) != (arm1, src1):
            continue  # cross-arm pair: skipped by design
        if h0 != h1:
            return fr
    return None


def _get_pixelhash_entry(log_path: str, frame: int):
    with open(log_path, "r", encoding="utf-8") as f:
        for line in f:
            j = json.loads(line)
            if j.get("kind") == "ui_replay_pixel_hash" and int(j.get("frame", -1)) == frame:
                return j
    return None


def _ranges(frames):
    frames = sorted(frames)
    out = []
    for _, g in groupby(enumerate(frames), lambda t: t[1] - t[0]):
        grp = [v for _, v in g]
        out.append((grp[0], grp[-1], len(grp)))
    return out


def generate_first_mismatch_diff(out_dir: str, scenario: str) -> int:
    base = os.path.join(out_dir, f"{scenario}.RelWithDebInfo")
    mode0_log = base + ".mode0.jsonl"
    mode1_log = base + ".mode1.jsonl"
    fr = _first_mismatch_frame(mode0_log, mode1_log)
    if fr is None:
        print(f"{scenario}: no mismatches")
        return 0

    e0 = _get_pixelhash_entry(mode0_log, fr)
    e1 = _get_pixelhash_entry(mode1_log, fr)
    if not e0 or not e1:
        print(f"{scenario}: missing pixel hash entry for frame {fr}")
        return 2

    x, y, w, h = int(e0["x"]), int(e0["y"]), int(e0["w"]), int(e0["h"])
    ppm0 = os.path.join(out_dir, f"{scenario}.RelWithDebInfo.mode0.frame{fr}.x{x}.y{y}.w{w}.h{h}.ppm")
    ppm1 = os.path.join(out_dir, f"{scenario}.RelWithDebInfo.mode1.frame{fr}.x{x}.y{y}.w{w}.h{h}.ppm")
    if not (os.path.exists(ppm0) and os.path.exists(ppm1)):
        print(f"{scenario}: missing PPMs for frame {fr}")
        print(f"  expected {os.path.basename(ppm0)}")
        print(f"  expected {os.path.basename(ppm1)}")
        return 3

    W, H, a = _read_ppm_p6(ppm0)
    W2, H2, b = _read_ppm_p6(ppm1)
    if (W, H) != (W2, H2):
        raise RuntimeError("ppm dimensions differ")

    # Also write the raw captures for easy inspection (no highlight overlay).
    raw0 = os.path.join(out_dir, f"diff_{scenario}.frame{fr}.mode0_raw.png")
    raw1 = os.path.join(out_dir, f"diff_{scenario}.frame{fr}.mode1_raw.png")
    _write_png_rgb8(raw0, W, H, a)
    _write_png_rgb8(raw1, W, H, b)

    def _black_stats(rgb: bytes):
        # Heuristic: if almost all pixels are near-black, the capture is likely invalid (black screen).
        # Returns (near_black_ratio, avg_luma_approx).
        near = 0
        s = 0
        npx = W * H
        for i in range(0, len(rgb), 3):
            r = rgb[i]
            g = rgb[i + 1]
            b_ = rgb[i + 2]
            # cheap luma-ish average
            l = (int(r) + int(g) + int(b_)) // 3
            s += l
            if l <= 2:
                near += 1
        return (near / max(1, npx), s / max(1, npx))

    nb0, avg0 = _black_stats(a)
    nb1, avg1 = _black_stats(b)

    na = bytearray(a)
    nb = bytearray(b)
    diff = bytearray(W * H * 3)
    mask = [0] * (W * H)
    minx = miny = 10**9
    maxx = maxy = -1
    changed = 0

    for i in range(W * H):
        ia = i * 3
        dr = na[ia] - nb[ia]
        dg = na[ia + 1] - nb[ia + 1]
        db = na[ia + 2] - nb[ia + 2]
        dr = dr if dr >= 0 else -dr
        dg = dg if dg >= 0 else -dg
        db = db if db >= 0 else -db
        d = max(dr, dg, db)
        if d:
            mask[i] = 1
            changed += 1
            px = i % W
            py = i // W
            minx = min(minx, px)
            miny = min(miny, py)
            maxx = max(maxx, px)
            maxy = max(maxy, py)
        v = d * 6
        if v > 255:
            v = 255
        diff[ia] = v
        diff[ia + 1] = v
        diff[ia + 2] = v

    ha = bytearray(na)
    hb = bytearray(nb)
    for i in range(W * H):
        if not mask[i]:
            continue
        ia = i * 3
        ha[ia] = 255
        ha[ia + 1] = 0
        ha[ia + 2] = 0
        hb[ia] = 255
        hb[ia + 1] = 0
        hb[ia + 2] = 0

    if changed and maxx >= minx and maxy >= miny:
        _draw_rect(ha, W, H, minx, miny, maxx, maxy, (255, 255, 0))
        _draw_rect(hb, W, H, minx, miny, maxx, maxy, (255, 255, 0))
        _draw_rect(diff, W, H, minx, miny, maxx, maxy, (255, 0, 255))

    out0 = os.path.join(out_dir, f"diff_{scenario}.frame{fr}.mode0_highlight.png")
    out1 = os.path.join(out_dir, f"diff_{scenario}.frame{fr}.mode1_highlight.png")
    outd = os.path.join(out_dir, f"diff_{scenario}.frame{fr}.diffmap.png")
    _write_png_rgb8(out0, W, H, ha)
    _write_png_rgb8(out1, W, H, hb)
    _write_png_rgb8(outd, W, H, diff)

    print(f"{scenario}: example_frame={fr} region=x{x} y{y} w{w} h{h} diff_pixels={changed}")
    print(f"  raw0 : {os.path.basename(raw0)} (near_black={nb0:.3f} avg_luma={avg0:.1f})")
    print(f"  raw1 : {os.path.basename(raw1)} (near_black={nb1:.3f} avg_luma={avg1:.1f})")
    print(f"  mode0: {os.path.basename(out0)}")
    print(f"  mode1: {os.path.basename(out1)}")
    print(f"  diff : {os.path.basename(outd)}")
    return 0


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--scenario", required=True)
    args = ap.parse_args()
    raise SystemExit(generate_first_mismatch_diff(args.out_dir, args.scenario))

