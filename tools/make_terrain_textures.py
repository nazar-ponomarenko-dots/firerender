#!/usr/bin/env python3
"""Generate SEAMLESS terrain material textures into img/.

Terrain splat-map layers (see src/splat.h):
    img/dirt.png     R — soil/dirt road (brown earth, clumps, pebbles)
    img/sand.png     G — sand (fine grain, no baked-in ripples)
    img/gravel.png   B — gravel/path (grey crushed stone, small rocks)
    img/asphalt.png  A — asphalt (dark, coarse aggregate, cracks, no lane markings)

All textures are 512x512 RGB and TILE seamlessly: the noise is built on a
lattice with periodic indexing (wrap by modulo), so the right edge continues
the left one and the bottom — the top.

Usage: python3 tools/make_terrain_textures.py
"""
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "img")
N = 512


def lattice(rng, n):
    """Periodic lattice of random values n x n."""
    return rng.random((n, n)).astype(np.float32)


def value_noise(rng, size, cells):
    """Seamless value-noise: a cells x cells lattice, bilinear interpolation
    with indices wrapped by modulo (the right edge joins the left one)."""
    g = lattice(rng, cells)
    # lattice coordinates
    t = np.linspace(0.0, cells, size, endpoint=False, dtype=np.float32)
    i0 = np.floor(t).astype(np.int32) % cells
    i1 = (i0 + 1) % cells
    f = t - np.floor(t)
    # smoothstep — soft transitions instead of linear "diamonds"
    f = f * f * (3.0 - 2.0 * f)
    fx = f[None, :]
    fy = f[:, None]
    g00 = g[np.ix_(i0, i0)]
    g10 = g[np.ix_(i0, i1)]
    g01 = g[np.ix_(i1, i0)]
    g11 = g[np.ix_(i1, i1)]
    top = g00 * (1 - fx) + g10 * fx
    bot = g01 * (1 - fx) + g11 * fx
    return top * (1 - fy) + bot * fy


def fbm(rng, size, base_cells, octaves, gain=0.5, lacunarity=2):
    """Fractal noise: sum of value-noise octaves (all periodic)."""
    out = np.zeros((size, size), dtype=np.float32)
    amp = 1.0
    total = 0.0
    cells = base_cells
    for _ in range(octaves):
        out += amp * value_noise(rng, size, cells)
        total += amp
        amp *= gain
        cells = int(cells * lacunarity)
    return out / total


def norm(a):
    lo, hi = float(a.min()), float(a.max())
    return (a - lo) / (hi - lo) if hi > lo else a * 0.0


def stones(rng, size, count, rmin, rmax, hard=1.0):
    """Periodic "pebbles": discs with a smoothed edge, wrapped at the borders."""
    m = np.zeros((size, size), dtype=np.float32)
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float32)
    for _ in range(count):
        cx, cy = rng.random(2) * size
        r = rmin + (rmax - rmin) * rng.random()
        # wrap: minimal distance taking periodicity into account
        dx = np.abs(xs - cx)
        dx = np.minimum(dx, size - dx)
        dy = np.abs(ys - cy)
        dy = np.minimum(dy, size - dy)
        d = np.sqrt(dx * dx + dy * dy)
        v = np.clip(1.0 - d / r, 0.0, 1.0) ** (0.6 / max(hard, 0.05))
        m = np.maximum(m, v * (0.55 + 0.45 * rng.random()))
    return m


def cracks(rng, size, count, width=1.6, seg=90):
    """Thin broken-line cracks (for asphalt). Periodic thanks to wrap coordinates."""
    m = np.zeros((size, size), dtype=np.float32)
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float32)
    for _ in range(count):
        px, py = rng.random(2) * size
        ang = rng.random() * 2 * np.pi
        for _ in range(seg):
            ang += (rng.random() - 0.5) * 0.9
            nx = px + np.cos(ang) * 6.0
            ny = py + np.sin(ang) * 6.0
            # segment (px,py)-(nx,ny): wrapped distance
            for t in np.linspace(0.0, 1.0, 5):
                sx = (px + (nx - px) * t) % size
                sy = (py + (ny - py) * t) % size
                dx = np.abs(xs - sx)
                dx = np.minimum(dx, size - dx)
                dy = np.abs(ys - sy)
                dy = np.minimum(dy, size - dy)
                d = np.sqrt(dx * dx + dy * dy)
                m = np.maximum(m, np.clip(1.0 - d / width, 0.0, 1.0))
            px, py = nx % size, ny % size
    return m


def save(path, rgb):
    img = np.clip(rgb * 255.0, 0, 255).astype(np.uint8)
    Image.fromarray(img).save(path)
    print(f"  + {os.path.relpath(path, ROOT)}  {img.shape[1]}x{img.shape[0]}")


def make_dirt():
    rng = np.random.default_rng(1701)
    clump = fbm(rng, N, 6, 5)                      # large clumps
    grain = fbm(rng, N, 64, 3)                     # grain
    peb = stones(rng, N, 220, 2.0, 5.5, hard=1.4)  # pebbles
    v = 0.62 * norm(clump) + 0.30 * grain + 0.22 * peb
    v = np.clip(v, 0, 1)
    # brown earth: warm dark tone, pebbles slightly lighter and greyer
    base = np.stack([0.35 + 0.30 * v, 0.25 + 0.24 * v, 0.16 + 0.17 * v], -1)
    grey = np.stack([peb, peb, peb], -1) * 0.22
    return np.clip(base + grey, 0, 1)


def make_sand():
    rng = np.random.default_rng(20240117)
    grain = fbm(rng, N, 96, 3)                     # fine grain
    dune = value_noise(rng, N, 5)                  # smooth swells
    # NO "dune ripples": a baked-in sine gave regular diagonal bands on flat
    # beaches when the texture tiled (fake dunes). The sand's relief comes from
    # the shader's detail normals and the large-scale dune.
    v = 0.62 * grain + 0.38 * norm(dune)
    v = np.clip(v, 0, 1)
    return np.clip(np.stack([0.74 + 0.22 * v,
                             0.66 + 0.22 * v,
                             0.47 + 0.21 * v], -1), 0, 1)


def make_gravel():
    rng = np.random.default_rng(90210)
    fine = fbm(rng, N, 80, 3)
    big = stones(rng, N, 900, 2.5, 7.0, hard=1.8)
    small = stones(rng, N, 1800, 1.2, 3.0, hard=2.4)
    v = np.clip(0.30 * fine + 0.62 * big + 0.38 * small, 0, 1)
    # grey-beige crushed stone, individual rocks cooler
    tone = 0.42 + 0.44 * v
    return np.clip(np.stack([tone * 1.03, tone * 1.0, tone * 0.94], -1), 0, 1)


def make_asphalt():
    rng = np.random.default_rng(555)
    fine = fbm(rng, N, 128, 3)                     # fine aggregate
    aggr = stones(rng, N, 2600, 1.0, 2.6, hard=3.0)  # gravel in bitumen
    patch = fbm(rng, N, 4, 3)                      # wear patches
    cr = cracks(rng, N, 5, width=1.5, seg=70)
    v = 0.34 * fine + 0.30 * aggr + 0.26 * norm(patch)
    tone = 0.17 + 0.24 * np.clip(v, 0, 1)
    tone = tone * (1.0 - 0.55 * cr)                # cracks — darker
    rgbv = np.stack([tone * 1.02, tone * 1.0, tone * 1.04], -1)
    return np.clip(rgbv, 0, 1)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    save(os.path.join(OUT, "dirt.png"), make_dirt())
    save(os.path.join(OUT, "sand.png"), make_sand())
    save(os.path.join(OUT, "gravel.png"), make_gravel())
    save(os.path.join(OUT, "asphalt.png"), make_asphalt())
    print("Terrain material textures ready (seamless 512x512).")
