"""Generates the parallax test project's textures: height, normal and albedo maps for a brick wall,
a cobbled ground and a tiled roof, all first-party and tileable.

Albedos are sRGB-encoded colours. Heights follow the engine's convention: 1 at the polygon surface,
0 at the deepest point. Each normal map is the slope of its own height map at the materials' relief
depth (a fraction of one texture repeat), in tangent space with +x along increasing u and +y along
increasing v (image rows downward), so the lighting and the relief agree.

Run from anywhere: python generate_textures.py
"""

from pathlib import Path

import numpy as np
from PIL import Image

SIZE = 512
RELIEF_DEPTH = 0.02  # the materials' relief depth, a fraction of one repeat
OUT = Path(__file__).resolve().parent / "Assets" / "Textures"


def smooth_noise(rng, cells, size=SIZE):
    """Tileable value noise: `cells` lattice cells per repeat, bicubic-smoothed, in [0, 1]."""
    lattice = rng.random((cells, cells))
    coords = np.arange(size) * cells / size
    base = np.floor(coords).astype(int)
    frac = coords - base
    fade = frac * frac * (3.0 - 2.0 * frac)
    i0, i1 = base % cells, (base + 1) % cells
    rows0 = lattice[i0][:, i0] * (1 - fade)[None, :] + lattice[i0][:, i1] * fade[None, :]
    rows1 = lattice[i1][:, i0] * (1 - fade)[None, :] + lattice[i1][:, i1] * fade[None, :]
    return rows0 * (1 - fade)[:, None] + rows1 * fade[:, None]


def fractal_noise(rng, base_cells, octaves=4):
    total, amplitude, norm = np.zeros((SIZE, SIZE)), 1.0, 0.0
    for octave in range(octaves):
        total += amplitude * smooth_noise(rng, base_cells * 2 ** octave)
        norm += amplitude
        amplitude *= 0.5
    return total / norm


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def normal_map(height):
    """Tangent-space normals of the relief `height` carves at RELIEF_DEPTH, wrapped at the edges."""
    du = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * SIZE * RELIEF_DEPTH
    dv = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * SIZE * RELIEF_DEPTH
    n = np.stack([-du, -dv, np.ones_like(height)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def save(name, array, mode):
    data = np.clip(np.round(array * 255.0), 0, 255).astype(np.uint8)
    Image.fromarray(data, mode).save(OUT / name, optimize=True)


def bricks(rng):
    """Running bond, 8 courses of 4 bricks per repeat, 2.5 % mortar joints, bevelled faces."""
    courses, per_course, joint, bevel = 8, 4, 0.025, 0.012
    y, x = np.mgrid[0:SIZE, 0:SIZE] / SIZE
    course = np.floor(y * courses).astype(int)
    shifted = (x + 0.5 / per_course * (course % 2)) % 1.0
    column = np.floor(shifted * per_course).astype(int)
    within_x = shifted * per_course - column
    within_y = y * courses - course
    edge = np.minimum(np.minimum(within_x, 1 - within_x) / per_course, np.minimum(within_y, 1 - within_y) / courses)
    face = smoothstep(joint * 0.5, joint * 0.5 + bevel, edge)
    grain = fractal_noise(rng, 16)
    height = 0.1 + 0.08 * grain + face * (0.82 - 0.1 * grain)

    brick_id = (course * per_course + column) % (courses * per_course)
    tint = rng.random((courses * per_course, 3)) * np.array([0.12, 0.06, 0.04])
    brick = np.array([0.6, 0.3, 0.2])[None, None, :] + tint[brick_id]
    brick *= (0.85 + 0.3 * fractal_noise(rng, 32))[..., None]
    mortar = np.array([0.74, 0.72, 0.68])[None, None, :] * (0.9 + 0.2 * grain)[..., None]
    albedo = brick * face[..., None] + mortar * (1 - face[..., None])
    return height, albedo


def cobbles(rng):
    """Rounded stones on a jittered 6 x 6 lattice per repeat, packed with gaps between them."""
    cells = 6
    seeds = (np.stack(np.meshgrid(np.arange(cells), np.arange(cells), indexing="ij"), -1).reshape(-1, 2)
             + 0.2 + 0.6 * rng.random((cells * cells, 2))) / cells
    y, x = np.mgrid[0:SIZE, 0:SIZE] / SIZE
    first = np.full((SIZE, SIZE), np.inf)
    second = np.full((SIZE, SIZE), np.inf)
    owner = np.zeros((SIZE, SIZE), dtype=int)
    for index, (sy, sx) in enumerate(seeds):
        dy = np.abs(y - sy)
        dx = np.abs(x - sx)
        distance = np.hypot(np.minimum(dx, 1 - dx), np.minimum(dy, 1 - dy))
        closer = distance < first
        second = np.where(closer, first, np.minimum(second, distance))
        owner = np.where(closer, index, owner)
        first = np.minimum(first, distance)
    edge = (second - first) * cells  # 0 on a border, about 1 at a stone's centre
    dome = np.sqrt(np.clip(edge / 0.45, 0.0, 1.0))
    grain = fractal_noise(rng, 24)
    height = np.clip(0.9 * dome * (0.9 + 0.1 * grain) + 0.05 * grain, 0.0, 1.0)

    tone = 0.5 + 0.14 * rng.random(cells * cells)
    stone = tone[owner][..., None] * np.array([1.0, 0.97, 0.9])[None, None, :]
    stone *= (0.85 + 0.3 * fractal_noise(rng, 48))[..., None]
    dirt = np.array([0.38, 0.33, 0.26])[None, None, :]
    mix = smoothstep(0.05, 0.3, dome)[..., None]
    return height, stone * mix + dirt * (1 - mix)


def roof_tiles(rng):
    """Overlapping rows of 6 tiles, 8 rows per repeat: each tile rises from under the row above to
    its lip, with a groove between neighbours."""
    rows, per_row, groove = 8, 6, 0.02
    y, x = np.mgrid[0:SIZE, 0:SIZE] / SIZE
    row = np.floor(y * rows).astype(int)
    shifted = (x + 0.5 / per_row * (row % 2)) % 1.0
    tile = np.floor(shifted * per_row).astype(int)
    within_x = shifted * per_row - tile
    within_y = y * rows - row
    rise = 0.35 + 0.65 * within_y ** 0.8
    lip = smoothstep(1.0, 0.94, within_y)
    side = smoothstep(groove * per_row * 0.5, groove * per_row * 1.5, np.minimum(within_x, 1 - within_x))
    grain = fractal_noise(rng, 20)
    height = np.clip(rise * lip * (0.35 + 0.65 * side) + 0.04 * grain, 0.0, 1.0)

    tile_id = (row * per_row + tile) % (rows * per_row)
    tint = rng.random(rows * per_row)
    terracotta = np.array([0.74, 0.38, 0.24])[None, None, :] * (0.8 + 0.3 * tint[tile_id])[..., None]
    terracotta *= (0.85 + 0.3 * fractal_noise(rng, 32))[..., None]
    shade = (0.55 + 0.45 * within_y)[..., None]
    return height, terracotta * shade


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, generator, seed in (("bricks", bricks, 7), ("cobbles", cobbles, 11), ("roof_tiles", roof_tiles, 13)):
        height, albedo = generator(np.random.default_rng(seed))
        save(f"{name}_height.png", height, "L")
        save(f"{name}_normal.png", normal_map(height), "RGB")
        save(f"{name}_albedo.png", albedo, "RGB")


if __name__ == "__main__":
    main()
