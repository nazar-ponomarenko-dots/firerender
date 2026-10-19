#!/usr/bin/env python3
"""Inject baked light into an EXISTING maps/<name>.zip without touching geometry.

Unlike make_map_zip.py (which rebuilds the zip from the map/ folder = map1 only),
this tool opens the finished map zip and replaces its baked/* entries with
fresh files from map/baked/. That is how `make bake-maps` works for ANY map:
the engine bakes light for the requested map (FR_MAP=<name>) into map/baked/,
and we put it back into exactly that map's zip.

Usage:  python3 tools/inject_baked.py <map-name>
"""
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP_NAME = sys.argv[1] if len(sys.argv) > 1 else "map1"
ZIP = os.path.join(ROOT, "maps", MAP_NAME + ".zip")
BAKED = os.path.join(ROOT, "map", "baked")

if not os.path.isfile(ZIP):
    print(f"no map: {ZIP}")
    sys.exit(1)
if not os.path.isdir(BAKED):
    print(f"no baked light: {BAKED}")
    sys.exit(1)

baked_files = sorted(f for f in os.listdir(BAKED)
                     if os.path.isfile(os.path.join(BAKED, f)))
baked_names = {"baked/" + f for f in baked_files}

# Re-read the existing zip, dropping the old baked/* entries, then append the fresh ones.
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
print(f"Baked light injected into {ZIP} ({os.path.getsize(ZIP)//1024} KB)")
