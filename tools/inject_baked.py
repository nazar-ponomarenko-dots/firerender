#!/usr/bin/env python3
"""Вкладывает запечённый свет в СУЩЕСТВУЮЩИЙ maps/<имя>.zip, не трогая геометрию.

В отличие от make_map_zip.py (пересобирает zip из папки map/ = только map1),
этот инструмент открывает готовый zip карты и заменяет в нём записи baked/*
свежими файлами из map/baked/. Так `make bake-maps` работает для ЛЮБОЙ карты:
движок запёк свет нужной карты (FR_MAP=<имя>) в map/baked/, а мы кладём его
обратно ровно в её zip.

Запуск:  python3 tools/inject_baked.py <имя-карты>
"""
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP_NAME = sys.argv[1] if len(sys.argv) > 1 else "map1"
ZIP = os.path.join(ROOT, "maps", MAP_NAME + ".zip")
BAKED = os.path.join(ROOT, "map", "baked")

if not os.path.isfile(ZIP):
    print(f"нет карты: {ZIP}")
    sys.exit(1)
if not os.path.isdir(BAKED):
    print(f"нет запечённого света: {BAKED}")
    sys.exit(1)

baked_files = sorted(f for f in os.listdir(BAKED)
                     if os.path.isfile(os.path.join(BAKED, f)))
baked_names = {"baked/" + f for f in baked_files}

# Перечитываем существующий zip, откидывая старые baked/*, затем дописываем свежие.
with zipfile.ZipFile(ZIP, "r") as zin:
    keep = [(i, zin.read(i.filename)) for i in zin.infolist()
            if i.filename not in baked_names]

tmp = ZIP + ".tmp"
with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zout:
    for info, data in keep:
        zout.writestr(info, data)
    for f in baked_files:
        with open(os.path.join(BAKED, f), "rb") as fh:
            zout.writestr("baked/" + f, fh.read())
        print(f"  ~ baked/{f}")

os.replace(tmp, ZIP)
print(f"Свет вложен в {ZIP} ({os.path.getsize(ZIP)//1024} КБ)")
