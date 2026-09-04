#!/usr/bin/env python3
"""Генерирует ГЛАДКУЮ тайлящуюся FBM-текстуру для облаков: img/cloud_noise.png

img/noise.png — белое зерно (снег/мокрота), облакам нужен плавный шум: 3 октавы
value-noise на периодических решётках (8/32/128 ячеек), интерполяция smoothstep
с обходом индексов по модулю — правый край продолжает левый без шва.
Запуск: /usr/bin/python3 tools/make_cloud_noise.py
"""
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "img", "cloud_noise.png")
N = 512


def lattice_noise(rng, cells):
    """Value-noise с решёткой cells x cells на холсте N x N, тайлится сам."""
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
    sy = wy * wy * (3.0 - 2.0 * wy)          # smoothstep: мягкие «клубы»
    sx = wx * wx * (3.0 - 2.0 * wx)
    a = g[y0, x0]
    b = g[y0, x1]
    c = g[y1, x0]
    d = g[y1, x1]
    top = a + (b - a) * sx
    bot = c + (d - c) * sx
    return top + (bot - top) * sy


def fbm(rng):
    """3 октавы: крупные клубы + средние + мелкая рябь кромки."""
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
    print(f"ok: {OUT} ({N}x{N}, гладкий тайлящийся FBM для облаков)")


if __name__ == "__main__":
    main()
