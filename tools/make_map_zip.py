#!/usr/bin/env python3
"""Собирает карту в maps/<имя>.zip.

Внутри zip — корень карты:
    map.json            манифест карты
    map.png             карта высот
    img/...             текстуры, на которые ссылается map.json
    (позже: models/, scripts/, sounds/)

Ресурсов, которых нет в zip, движок подсосёт из img/ движка.
Запуск:  python3 tools/make_map_zip.py [имя]
"""
import json
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP_NAME = sys.argv[1] if len(sys.argv) > 1 else "map1"
MAP_DIR = os.path.join(ROOT, "map")
OUT = os.path.join(ROOT, "maps", MAP_NAME + ".zip")

# All relative paths referenced by map.json
refs = []
with open(os.path.join(MAP_DIR, "map.json"), "r", encoding="utf-8") as f:
    data = json.load(f)

    def walk(o):
        if isinstance(o, dict):
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
        elif isinstance(o, str) and o.endswith((".png", ".jpg", ".jpeg",
                                                ".ttf", ".gltf", ".glb", ".obj",
                                                ".wav", ".vs", ".fs")):
            refs.append(o)
    walk(data)

# Required files — from map/ (stored at the zip ROOT)
files = [("map.json", os.path.join(MAP_DIR, "map.json")),
         ("map.png", os.path.join(MAP_DIR, "map.png"))]
# Map resources — keep their relative paths (img/...). Look first in the
# engine root (shared textures/shaders), then in the map folder map/ —
# a map may carry its own resources (shaders/, img/, audio/).
# map.json/map.png were already added above: skip them, otherwise the zip
# would get duplicate names (map.png is also in the manifest as terrain.image).
added = {name for name, _ in files}
for rel in sorted(set(refs)):
    if rel in added:
        continue
    src = os.path.join(ROOT, rel)
    if not os.path.isfile(src):
        src = os.path.join(MAP_DIR, rel)
    if os.path.isfile(src):
        files.append((rel, src))
        added.add(rel)

# Baked light (make mapbake): everything in map/baked/ goes into the zip as baked/*.
# The engine reads baked/lightmap.png and baked/probes.bin (see terrain.c).
baked_dir = os.path.join(MAP_DIR, "baked")
if os.path.isdir(baked_dir):
    for fn in sorted(os.listdir(baked_dir)):
        src = os.path.join(baked_dir, fn)
        if os.path.isfile(src):
            files.append(("baked/" + fn, src))

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
    for name, src in files:
        z.write(src, name)
        print(f"  + {name}")

print(f"Карта собрана: {OUT} ({os.path.getsize(OUT)//1024} КБ)")
