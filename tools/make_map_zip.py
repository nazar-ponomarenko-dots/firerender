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

# Все относительные пути, на которые ссылается map.json
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

# Обязательные файлы — из map/ (в zip кладутся в КОРЕНЬ)
files = [("map.json", os.path.join(MAP_DIR, "map.json")),
         ("map.png", os.path.join(MAP_DIR, "map.png"))]
# Ресурсы карты — с их относительными путями (img/...). Сначала ищем в
# корне движка (общие текстуры/шейдеры), потом в папке карты map/ —
# карта может нести свои ресурсы (shaders/, img/, audio/).
# map.json/map.png уже добавлены выше: пропускаем, иначе zip получал
# дубликат имени (map.png лежит и в манифесте как terrain.image).
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

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
    for name, src in files:
        z.write(src, name)
        print(f"  + {name}")

print(f"Карта собрана: {OUT} ({os.path.getsize(OUT)//1024} КБ)")
