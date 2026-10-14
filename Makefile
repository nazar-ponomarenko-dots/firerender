# ============================================================================
#  Makefile for firerender (SDL3 + OpenGL, pure C)
# ----------------------------------------------------------------------------
#  Project layout:
#    src/           — sources (main.c + modules: gl_loader, math, world,
#                      mapdata, json, scene, camera, sun, sky, shadow,
#                      terrain, vegetation, field, minimap, hud, shader,
#                      assets)
#    map/           — map data: map.png + map.json (field, lighting, objects)
#    shaders/       — GLSL shaders (loaded from disk)
#    third_party/   — third-party libraries (stb_image, 8x8 font)
#    build/         — built binary
#  Targets:
#    make          — build the binary ./build/firerender
#    make run      — build and run
#    make clean    — delete the binary
#    make distclean— delete everything (build/)
# ----------------------------------------------------------------------------
#  SDL3 comes from the system via pkg-config (version 3.x required).
#  OpenGL functions are loaded at runtime through
#  SDL_GL_GetProcAddress, so no -lGL / -lGLEW / -lEGL is needed:
#  SDL itself links the right GL (GLX on X11, EGL on Wayland/Android,
#  WGL on Windows, CGL on macOS). The code works on all these platforms.
#  To build on other OSes it is enough to install SDL3 (or build it)
#  and run make — the sources need no changes.
# ============================================================================

# Python interpreter for map packing/baking. Defaults to python3, but
# on this machine the system /usr/bin/python3 works while python3 from the
# AppImage is broken; override it as: make PYTHON=/usr/bin/python3
PYTHON ?= python3

CC   := gcc
#   -O2            — level 2 optimization
#   -Wall -Wextra  — all major warnings (very useful in C)
#   -Isrc          — so that #include "camera.h" and the like resolve inside src/
#   pkg-config     — include/lib flags for the system SDL3
# Lua is vendored (third_party/lua): lua.c amalgamation + 5.4.8 headers —
# the build is self-contained and does not depend on a system-installed lua
# (important for the linux/win cross-builds).
CFLAGS := -O2 -Wall -Wextra -Ithird_party -Ithird_party/lua -Ithird_party/miniz -Isrc $(shell pkg-config --cflags sdl3)
LIBS   := $(shell pkg-config --libs sdl3) -lm   # -lm: math library

# Extra compiler flags (example: EXTRA_CFLAGS="-fsanitize=address,undefined" make)
CFLAGS += $(EXTRA_CFLAGS)

# Sources and binary ---------------------------------------------------------
SRC := $(wildcard src/*.c) \
       third_party/lua/lua.c \
       third_party/miniz/miniz.c third_party/miniz/miniz_zip.c \
       third_party/miniz/miniz_tinfl.c third_party/miniz/miniz_tdef.c
BIN := build/firerender
GLES_BIN := build/firerender-gles

# The first target in the file is buildnum (phony); so that «make» builds the engine AND
# shadows, the default goal is pinned to all (see below: engine + baking all maps).
.DEFAULT_GOAL := all

# --- Build number ------------------------------------------------------------
# The idea is as in engines: each build gets a sequential number, which tells
# one engine version from another (console: version, startup logs).
# buildnum.txt is the counter (local, in .gitignore); make increments it on
# EVERY build and writes src/buildnum.h with #define ENGINE_BUILD_NUM n.
BUILD_COUNT := buildnum.txt
BUILD_H     := src/buildnum.h

.PHONY: buildnum
buildnum:
	@num=0; [ -f $(BUILD_COUNT) ] && num=$$(cat $(BUILD_COUNT) 2>/dev/null); \
	  num=$$((num + 1)); \
	  printf '%d' $$num > $(BUILD_COUNT); \
	  printf '#ifndef FR_BUILDNUM_H\n#define FR_BUILDNUM_H\n#define ENGINE_BUILD_NUM %d\n#endif\n' $$num > $(BUILD_H); \
	  echo "Build #$$num"

# Build rule: if any source is newer than the binary — rebuild.
# buildnum (phony) increments the build number and writes buildnum.h.
$(BIN): $(SRC) buildnum
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LIBS)

# Run. No forced SDL_VIDEO_DRIVER: SDL itself picks the
# best backend (on a Wayland session that is native Wayland + EGL).
run: $(BIN)
	./$(BIN)

# Second renderer profile: OpenGL ES 3.0 via SDL/EGL. Needs the system
# GLES3 headers; linking stays through SDL, as with desktop GL.
gles:
	@mkdir -p build
	$(CC) $(CFLAGS) -DUSE_GLES $(SRC) -o $(GLES_BIN) $(LIBS)

run-gles: gles
	./$(GLES_BIN)

# --- Portable Linux binary (all resources embedded, SDL3 static) --------------
# ./firerender is self-contained: map/, img/, shaders/ are embedded in the exe (no
# side files needed), SDL3 is linked statically, glibc symbols at version 2.28 —
# runs on any Linux with glibc >= 2.28 (all modern Linux).
# Built through zig (downloaded by itself on first run) — no root and no
# installed SDL3/junk in the system.
ZIG_DIR ?= /tmp/opencode/zig
ZIG_WRAPPER := /tmp/opencode/zigcc28
SDL3_G28 := build/sdl3-g28
SDL3_G28_PREFIX := build/sdl3-g28-prefix

$(SDL3_G28_PREFIX)/lib/libSDL3.a:
	@mkdir -p /tmp/opencode
	@test -x $(ZIG_DIR)/zig || (echo "Скачиваю zig..." && \
	  curl -sL -o /tmp/opencode/zig.tar.xz \
	    https://ziglang.org/download/0.15.2/zig-x86_64-linux-0.15.2.tar.xz && \
	  tar xf /tmp/opencode/zig.tar.xz -C /tmp/opencode && \
	  mv /tmp/opencode/zig-x86_64-linux-0.15.2 $(ZIG_DIR))
	@printf '#!/bin/sh\nexec %s/zig cc -target x86_64-linux-gnu.2.28 "$$@"\n' \
	  $(ZIG_DIR) > $(ZIG_WRAPPER)
	@chmod +x $(ZIG_WRAPPER)
	cmake -S third_party/SDL -B $(SDL3_G28) -DBUILD_SHARED_LIBS=OFF \
	  -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF \
	  -DCMAKE_C_COMPILER=$(ZIG_WRAPPER) -DCMAKE_C_FLAGS="-isystem /usr/include" \
	  -DCMAKE_INSTALL_PREFIX=$(SDL3_G28_PREFIX)
	cmake --build $(SDL3_G28) -j$$(nproc)
	cmake --install $(SDL3_G28)

linux: $(SDL3_G28_PREFIX)/lib/libSDL3.a
	$(ZIG_DIR)/zig cc -target x86_64-linux-gnu.2.28 -O2 \
	  -Ithird_party -Ithird_party/lua -Ithird_party/miniz -Isrc -I$(SDL3_G28_PREFIX)/include -isystem /usr/include \
	  src/*.c third_party/lua/lua.c third_party/miniz/miniz.c third_party/miniz/miniz_zip.c \
	  third_party/miniz/miniz_tinfl.c third_party/miniz/miniz_tdef.c \
	  -o build/firerender-linux -L$(SDL3_G28_PREFIX)/lib \
	  -lSDL3 -lm -lpthread -ldl
	strip build/firerender-linux
	cp build/firerender-linux ./firerender
	@echo "Портативный бинарник готов: ./firerender"

# --- Cross-build Windows exe (x86_64) ----------------------------------------
# Needs: x86_64-w64-mingw32-gcc and a static SDL3 built for mingw.
#   SDL3WIN_INC — include folder containing SDL3/ (default /tmp/opencode/sdl3-install/include)
#   SDL3WIN_LIB — path to libSDL3.a   (default .../lib/libSDL3.a)
# Example: SDL3WIN_INC=... SDL3WIN_LIB=... make win
SDL3WIN_ROOT ?= /tmp/opencode/sdl3-install
SDL3WIN_INC  ?= $(SDL3WIN_ROOT)/include
SDL3WIN_LIB  ?= $(SDL3WIN_ROOT)/lib/libSDL3.a
# SDL3 is static: link in everything it needs (opengl32, winmm, d3d, etc.).
# -mwindows — GUI app without a console; -static — everything into one exe.
win:
	@mkdir -p build
	x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -mwindows \
		-Isrc -Ithird_party -Ithird_party/lua -Ithird_party/miniz -I$(SDL3WIN_INC) \
		$(SRC) -o build/firerender.exe \
		$(SDL3WIN_LIB) \
		-lopengl32 -lwinmm -luser32 -lgdi32 -limm32 -lole32 -loleaut32 \
		-lshell32 -lversion -luuid -lsetupapi -lws2_32 -ldwmapi -lbcrypt \
		-static -static-libgcc

# Package for Windows: exe + data folders alongside (map/, img/, shaders/, lua/, audio/).
# The engine embeds no data — resources are searched in the map pak zip, then on disk
# (see resources.c: pak -> disk -> base_dir -> ../).
winpkg: win
	rm -rf build/win
	mkdir -p build/win
	cp build/firerender.exe build/win/
	@echo "Готово: build/win/firerender.exe — нужны папки ресурсов рядом или выше"

# Rebuild the map into maps/<name>.zip (map1 by default): map.json, map.png,
# all map resources. The map is chosen in the menu or by the command map <name>.
mapzip:
	$(PYTHON) tools/make_map_zip.py $(MAP)

# Bake the map lighting offline (map compilation): computes lighting in 3D with all
# bounces and AO, writes map/baked/lightmap.png + probes.bin, then
# repacks the map zip. The engine only reads it at runtime (cvar r_baked_light).
# Requires the built $(BIN). Runs headless (FR_TEST=S loads the world).
mapbake: $(BIN)
	FR_BAKE=1 FR_TEST=S SDL_VIDEODRIVER=$${SDL_VIDEODRIVER:-dummy} ./$(BIN) || true
	$(PYTHON) tools/make_map_zip.py $(MAP)
	@echo "Свет карты запечён и упакован в maps/$(if $(MAP),$(MAP),map1).zip"

# Bake lighting + shadows for ALL maps maps/*.zip. For each map the engine loads
# exactly that one (FR_MAP=<name>), computes lightmap+sunvis+probes into map/baked/, then
# tools/inject_baked.py puts the fresh baked/* back into its zip (the map geometry
# is untouched — unlike make_map_zip.py, which rebuilds from the map/ folder).
MAPS := $(patsubst maps/%.zip,%,$(wildcard maps/*.zip))
BAKE_STAMP := build/.baked.stamp

# Shared bake loop: for each map — the engine with FR_MAP + the baked/* step.
define BAKE_ALL_MAPS
	@for m in $(MAPS); do \
	  echo "== запекаю свет карты $$m =="; \
	  FR_BAKE=1 FR_MAP=$$m FR_TEST=S SDL_VIDEODRIVER=$${SDL_VIDEODRIVER:-offscreen} ./$(BIN) || true; \
	  $(PYTHON) tools/inject_baked.py $$m; \
	done
endef

.PHONY: bake-maps
bake-maps: $(BIN)
	$(BAKE_ALL_MAPS)
	@echo "Тени всех карт пересобраны: $(MAPS)"

# Stamp: building the engine drags along a rebuild of all maps' shadows (see target all).
# $(BIN) within one «make» is built exactly once — there is no second
# build/number increment here (the loop calls the ready binary, does not rebuild).
$(BAKE_STAMP): $(BIN)
	$(BAKE_ALL_MAPS)
	@mkdir -p build && touch $@

# Default target: build the engine AND rebuild shadows of all maps (BAKE=1).
# Fast iteration without baking:  make BAKE=0   or   make $(BIN)
BAKE ?= 1
.PHONY: all
all: $(BIN)
ifeq ($(BAKE),1)
all: $(BAKE_STAMP)
endif


clean:
	rm -f $(BIN)

# Full clean: removes both the binary and build/ entirely.
distclean:
	rm -rf build

# --- Android APK (one command; see tools/make_apk.sh) ------------------------
# Needs: Android SDK (/opt/android-sdk by default or ANDROID_SDK=...), NDK
# r28+, build-tools 36. Resources are packed into the APK as assets and unpacked
# on the device at first launch. ABI: arm64-v8a + x86_64 (emulator);
# narrow it: FR_ABIS="arm64-v8a" make apk
apk:
	ANDROID_SDK=$(if $(ANDROID_SDK),$(ANDROID_SDK),/opt/android-sdk) tools/make_apk.sh
	@echo "Готово: build/android/firerender.apk"

# .PHONY — these targets are not files, they must always run.
.PHONY: run gles run-gles clean distclean linux win winpkg apk buildnum

# --- Render backend status (informational) ----------------------------------
# The engine is multi-API by architecture: all GPU code goes through sr_* (srender.h).
# Today: OpenGL (make) and GLES (make gles). Vulkan/Metal — embryos in
# src/srender_stub.c; enabled by implementing srender_vk.c / srender_metal.c
# and the flags -DSR_ENABLE_VULKAN=1 / -DSR_ENABLE_METAL=1.
#   Vulkan: shaders GLSL -> SPIR-V (glslc --target-env=vulkan1.1),
#           surface via SDL_Vulkan_CreateSurface.
#   Metal:  MoltenVK (SPIR-V as is) or SPIRV-Cross -> MSL + SDL_Metal_CreateView.
backends:
	@echo "Готовые бэкенды:      OpenGL (make), GLES 3.x (make gles)"
	@echo "Заготовки (stubs):    Vulkan, Metal, Null — src/srender_stub.c"
	@echo "Путь включения:       srender_vk.c / srender_metal.c + -DSR_ENABLE_*=1"
	@echo "Статус в игре:        лог старта 'Render API: ...' (sr_api_report)"

# --- Experimental backends (make vulkan / make metal) ------------------------
# Skeleton: sr_create() brings up a Vulkan device (srender_vk.c) / Metal skeleton
# (srender_metal.c, __APPLE__ only); srender_gl.c is excluded. The render
# pipeline is not ported to these APIs yet — the binary brings up the device and
# exits with a clear log. Build the macOS target only on Apple.
VK_SRC := $(filter-out src/srender_gl.c,$(SRC))

vulkan: buildnum
	@mkdir -p build
	$(CC) $(CFLAGS) -DFR_BACKEND_VULKAN -DSR_ENABLE_VULKAN=1 $(VK_SRC) -o build/firerender-vulkan $(LIBS)

run-vulkan: vulkan
	./build/firerender-vulkan

metal: buildnum
	@mkdir -p build
	@uname -s | grep -q Darwin || { echo "Metal-цель собирается только на macOS"; exit 1; }
	$(CC) $(CFLAGS) -DFR_BACKEND_METAL -DSR_ENABLE_METAL=1 $(VK_SRC) -o build/firerender-metal $(LIBS)
