# ============================================================================
#  Makefile для firerender (SDL3 + OpenGL, чистый C)
# ----------------------------------------------------------------------------
#  Структура проекта:
#    src/           — исходники (main.c + модули: gl_loader, math, world,
#                      mapdata, json, scene, camera, sun, sky, shadow,
#                      terrain, vegetation, field, minimap, hud, shader,
#                      assets)
#    map/           — данные карты: map.png + map.json (поле, свет, объекты)
#    shaders/       — GLSL-шейдеры (загружаются с диска)
#    third_party/   — сторонние библиотеки (stb_image, шрифт 8x8)
#    build/         — собранный бинарник
#  Цели:
#    make          — собрать бинарник ./build/firerender
#    make run      — собрать и запустить
#    make clean    — удалить бинарник
#    make distclean— удалить всё (build/)
# ----------------------------------------------------------------------------
#  SDL3 берётся из системы через pkg-config (нужна версия 3.x).
#  OpenGL-функции подгружаются во время выполнения через
#  SDL_GL_GetProcAddress, поэтому никакие -lGL / -lGLEW / -lEGL не нужны:
#  SDL сам линкует нужный GL (GLX на X11, EGL на Wayland/Android,
#  WGL на Windows, CGL на macOS). Код работает на всех этих платформах.
#  Для сборки на других ОС достаточно установить SDL3 (или собрать его)
#  и запустить make — исходники менять не нужно.
# ============================================================================

CC   := gcc
#   -O2            — оптимизация уровня 2
#   -Wall -Wextra  — все основные предупреждения (очень полезно в C)
#   -Isrc          — чтобы #include "camera.h" и т.п. находились в src/
#   pkg-config     — флаги include/lib для SDL3 из системы
CFLAGS := -O2 -Wall -Wextra -Ithird_party -Ithird_party/miniz -Isrc $(shell pkg-config --cflags sdl3)
LIBS   := $(shell pkg-config --libs sdl3) -lm   # -lm: математика (sin/cos/sqrt)

# Исходники и бинарник ---------------------------------------------------------
SRC := $(wildcard src/*.c) \
       third_party/miniz/miniz.c third_party/miniz/miniz_zip.c \
       third_party/miniz/miniz_tinfl.c third_party/miniz/miniz_tdef.c
BIN := build/firerender

# Правило сборки: если любой исходник новее бинарника — пересобрать.
$(BIN): $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LIBS)

# Запуск. Никаких принудительных SDL_VIDEO_DRIVER: SDL сам выбирает
# лучший бэкенд (на Wayland-сессии это будет родной Wayland + EGL).
run: $(BIN)
	./$(BIN)

# --- Портативный Linux-бинарник (все ресурсы вшиты, SDL3 статически) ----------
# ./firerender — самодостаточный: map/, img/, shaders/ вшиты в exe (не нужны
# файлы рядом), SDL3 слинкован статически, glibc-символы версии 2.28 —
# запускается на любом Linux с glibc >= 2.28 (весь современный Linux).
# Сборка через zig (скачивается сам при первом запуске) — без root и без
# установленных SDL3/мусора в системе.
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
	  -Ithird_party -Ithird_party/miniz -Isrc -I$(SDL3_G28_PREFIX)/include -isystem /usr/include \
	  src/*.c third_party/miniz/miniz.c third_party/miniz/miniz_zip.c \
	  third_party/miniz/miniz_tinfl.c third_party/miniz/miniz_tdef.c \
	  -o build/firerender-linux -L$(SDL3_G28_PREFIX)/lib \
	  -lSDL3 -lm -lpthread -ldl
	strip build/firerender-linux
	cp build/firerender-linux ./firerender
	@echo "Портативный бинарник готов: ./firerender"

# --- Кросс-сборка Windows exe (x86_64) ----------------------------------------
# Нужны: x86_64-w64-mingw32-gcc и статический SDL3, собранный для mingw.
#   SDL3WIN_INC — папка include с SDL3/ (по умолчанию /tmp/opencode/sdl3-install/include)
#   SDL3WIN_LIB — путь к libSDL3.a   (по умолчанию .../lib/libSDL3.a)
# Пример: SDL3WIN_INC=... SDL3WIN_LIB=... make win
SDL3WIN_ROOT ?= /tmp/opencode/sdl3-install
SDL3WIN_INC  ?= $(SDL3WIN_ROOT)/include
SDL3WIN_LIB  ?= $(SDL3WIN_ROOT)/lib/libSDL3.a
# SDL3 статический: привязываем всё, что ему нужно (opengl32, winmm, d3d и пр.).
# -mwindows — GUI-приложение без консоли; -static — всё в один exe.
win:
	@mkdir -p build
	x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -mwindows \
		-Isrc -Ithird_party -Ithird_party/miniz -I$(SDL3WIN_INC) \
		$(SRC) -o build/firerender.exe \
		$(SDL3WIN_LIB) \
		-lopengl32 -lwinmm -luser32 -lgdi32 -limm32 -lole32 -loleaut32 \
		-lshell32 -lversion -luuid -lsetupapi -lws2_32 -ldwmapi -lbcrypt \
		-static -static-libgcc

# Пакет для Windows: ОДИН exe — все данные (map/, img/, shaders/) вшиты
# в бинарник (см. tools/embed_data.py), файлы рядом не нужны.
winpkg: win
	rm -rf build/win
	mkdir -p build/win
	cp build/firerender.exe build/win/
	@echo "Готово: build/win/firerender.exe — самодостаточный (запускайте откуда угодно)"

# Пересобрать карту в maps/<имя>.zip (по умолчанию map1): map.json, map.png,
# все ресурсы карты. Карту выбирают в меню или командой map <имя>.
mapzip:
	python3 tools/make_map_zip.py $(MAP)

clean:
	rm -f $(BIN)

# Полная очистка: удаляет и бинарник, и build/ целиком.
distclean:
	rm -rf build

# .PHONY — эти цели не являются файлами, их нужно выполнять всегда.
.PHONY: run clean distclean linux win winpkg
