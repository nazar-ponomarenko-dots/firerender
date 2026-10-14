#!/usr/bin/env python3
"""Generate a SMOOTH tileable FBM texture for clouds: img/cloud_noise.png

img/noise.png is white grain (snow/slush); clouds need a soft noise: 3 octaves
of value-noise on periodic lattices (8/32/128 cells), smoothstep interpolation
with indices wrapped by modulo — the right edge continues the left without a seam.
Usage: /usr/bin/python3 tools/make_cloud_noise.py
"""
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "img", "cloud_noise.png")
N = 512


def lattice_noise(rng, cells):
    """Value-noise on a cells x cells lattice over an N x N canvas, tiles seamlessly."""
    g = rng.random((cells, cells)).astype(np.float32)
    ys, xs = np.mgrid[0:N, 0:N].astype(np.float32)
    fy = ys * cells / N
    fx = xs * cells / N
    y0 = np.floor(fy).astype(int) % cells
    x0 = np.floor(fx).astype(int) % cells
    y1 = (y0 + 1) % cells
    x1 = (x0 + 1) % cells
    wy = fy - np.floor(fy)
    wx = fx - np.floor(fx)
    sy = wy * wy * (3.0 - 2.0 * wy)          # smoothstep: soft "clumps"
    sx = wx * wx * (3.0 - 2.0 * wx)
    a = g[y0, x0]
    b = g[y0, x1]
    c = g[y1, x0]
    d = g[y1, x1]
    top = a + (b - a) * sx
    bot = c + (d - c) * sx
    return top + (bot - top) * sy


def fbm(rng):
    """3 octaves: large billows + mid-size + fine edge ripple."""
    v = lattice_noise(rng, 8) * 0.55
    v += lattice_noise(rng, 32) * 0.30
    v += lattice_noise(rng, 128) * 0.15
    v -= v.min()
    return v / max(v.max(), 1e-6)


def main():
    rng = np.random.default_rng(777)
    v = fbm(rng)
    img = (np.clip(v, 0, 1) * 255).astype(np.uint8)
    Image.fromarray(img, "L").save(OUT)
    print(f"ok: {OUT} ({N}x{N}, smooth tileable FBM for clouds)")


if __name__ == "__main__":
    main()
