#!/usr/bin/env python3
"""Генерирует текстуры фауны (светлячки, бабочки, птицы) в img/.

Запуск: python3 tools/make_fauna_textures.py
"""
import math
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "img")


def write_png(path, w, h, rgba):
    raw = b""
    for y in range(h):
        raw += b"\x00"
        for x in range(w):
            r, g, b, a = rgba[y * w + x]
            raw += bytes((r, g, b, a))
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        c += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        return c
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    print("  +", os.path.relpath(path, ROOT))


def glow(w, h):
    """Радиальный градиент: центр яркий тёплый -> прозрачный край."""
    buf = []
    cx, cy = (w - 1) / 2, (h - 1) / 2
    for y in range(h):
        for x in range(w):
            d = math.hypot(x - cx, y - cy) / (w / 2)
            a = max(0.0, 1.0 - d)
            a = a * a * (3 - 2 * a)
            buf.append((255, 220, 150, int(255 * a)))
    return buf


def butterfly(w, h):
    """Бабочка сверху: два крыла-эллипса с тёмной окантовкой, тело."""
    buf = []
    for y in range(h):
        for x in range(w):
            u = x / w - 0.5
            v = y / h - 0.5
            # крылья: левое и правое (эллипсы, разнесённые от оси)
            wing = 0.0
            for sx in (-1, 1):
                wx = u * 2.0 - sx * 0.28
                wy = v * 2.0
                d = (wx * wx) / (0.34 * 0.34) + (wy * wy) / (0.42 * 0.42)
                if d <= 1.0:
                    wing = max(wing, 1.0 - d)
            # тело
            body = 0.0
            db = (u * 2.0) ** 2 / (0.045 ** 2) + (v * 2.0) ** 2 / (0.35 ** 2)
            if db <= 1.0:
                body = 1.0 - db
            # пятнышки на крыльях (для текстуры)
            speck = 0.0
            for sx in (-1, 1):
                wx = u * 2.0 - sx * 0.30
                wy = v * 2.0 + 0.05
                if (wx * wx) / (0.06 ** 2) + (wy * wy) / (0.07 ** 2) <= 1.0:
                    speck = 0.9
            a = max(wing, body)
            if a <= 0.01:
                buf.append((0, 0, 0, 0))
                continue
            r = int(180 + 40 * (1 - a)) if wing else 60
            g = int(140 + 30 * (1 - a)) if wing else 50
            b = int(90 + 60 * (1 - a)) if wing else 40
            if speck > 0:
                r, g, b = 40, 30, 25
            buf.append((r, g, b, int(255 * min(1.0, a * 1.5))))
    return buf


def bird(w, h):
    """Птица сбоку: корпус + распахнутые крылья (силуэт)."""
    buf = []
    for y in range(h):
        for x in range(w):
            u = x / w - 0.5
            v = y / h - 0.5
            # крылья: две дуги вверх-в стороны
            wl = 0.0
            for sx in (-1, 1):
                wx = u * 2.0 - sx * 0.18
                wy = (v * 2.0 - 0.18) * 1.2
                d = (wx * wx) / (0.38 * 0.38) + (wy * wy) / (0.22 * 0.22)
                if d <= 1.0 and wy < 0.25:
                    wl = max(wl, 1.0 - d)
            # корпус — вытянутый эллипс по X
            body = 0.0
            db = (u * 2.0) ** 2 / (0.40 ** 2) + (v * 2.0 - 0.10) ** 2 / (0.12 ** 2)
            if db <= 1.0:
                body = 1.0 - db
            a = max(wl, body)
            if a <= 0.01:
                buf.append((0, 0, 0, 0))
                continue
            buf.append((30, 30, 34, int(255 * min(1.0, a * 1.6))))
    return buf


def main():
    os.makedirs(OUT, exist_ok=True)
    write_png(os.path.join(OUT, "fauna_glow.png"), 64, 64, glow(64, 64))
    write_png(os.path.join(OUT, "fauna_butterfly.png"), 128, 128, butterfly(128, 128))
    write_png(os.path.join(OUT, "fauna_bird.png"), 128, 64, bird(128, 64))
    print("Готово.")


if __name__ == "__main__":
    main()
