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

# Интерпретатор Python для упаковки карт/запекания. По умолчанию python3, но
# на этой машине системный /usr/bin/python3 рабочий, а python3 из AppImage —
# битый; переопределяется так: make PYTHON=/usr/bin/python3
PYTHON ?= python3

CC   := gcc
#   -O2            — оптимизация уровня 2
#   -Wall -Wextra  — все основные предупреждения (очень полезно в C)
#   -Isrc          — чтобы #include "camera.h" и т.п. находились в src/
#   pkg-config     — флаги include/lib для SDL3 из системы
# Lua вендорен (third_party/lua): амальгамация lua.c + заголовки 5.4.8 —
# сборка самодостаточна и не зависит от установленного lua в системе
# (важно для кросс-сборок linux/win).
CFLAGS := -O2 -Wall -Wextra -Ithird_party -Ithird_party/lua -Ithird_party/miniz -Isrc $(shell pkg-config --cflags sdl3)
LIBS   := $(shell pkg-config --libs sdl3) -lm   # -lm: математика

# Дополнительные флаги компилятора (пример: EXTRA_CFLAGS="-fsanitize=address,undefined" make)
CFLAGS += $(EXTRA_CFLAGS)

# Исходники и бинарник ---------------------------------------------------------
SRC := $(wildcard src/*.c) \
       third_party/lua/lua.c \
       third_party/miniz/miniz.c third_party/miniz/miniz_zip.c \
       third_party/miniz/miniz_tinfl.c third_party/miniz/miniz_tdef.c
BIN := build/firerender
GLES_BIN := build/firerender-gles

# Первая цель в файле — buildnum (phony); чтобы «make» собирал движок И тени,
# фиксируем цель по умолчанию на all (см. ниже: движок + запекание всех карт).
.DEFAULT_GOAL := all

# --- Номер сборки (build number) ----------------------------------------------
# Идея как в движках: каждой сборке присваивается порядковый номер, по нему
# отличают одну версию движка от другой (консоль: version, логи при старте).
# buildnum.txt — счётчик (локальный, в .gitignore); make на КАЖДЫЙ сборку
# инкрементирует его и пишет src/buildnum.h с #define ENGINE_BUILD_NUM n.
BUILD_COUNT := buildnum.txt
BUILD_H     := src/buildnum.h

.PHONY: buildnum
buildnum:
	@num=0; [ -f $(BUILD_COUNT) ] && num=$$(cat $(BUILD_COUNT) 2>/dev/null); \
	  num=$$((num + 1)); \
	  printf '%d' $$num > $(BUILD_COUNT); \
	  printf '#ifndef FR_BUILDNUM_H\n#define FR_BUILDNUM_H\n#define ENGINE_BUILD_NUM %d\n#endif\n' $$num > $(BUILD_H); \
	  echo "Build #$$num"

# Правило сборки: если любой исходник новее бинарника — пересобрать.
# buildnum (phony) инкрементирует номер сборки и пишет buildnum.h.
$(BIN): $(SRC) buildnum
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LIBS)

# Запуск. Никаких принудительных SDL_VIDEO_DRIVER: SDL сам выбирает
# лучший бэкенд (на Wayland-сессии это будет родной Wayland + EGL).
run: $(BIN)
	./$(BIN)

# Второй renderer-профиль: OpenGL ES 3.0 через SDL/EGL. Нужны системные
# GLES3-заголовки; линковка остаётся через SDL, как у desktop GL.
gles:
	@mkdir -p build
	$(CC) $(CFLAGS) -DUSE_GLES $(SRC) -o $(GLES_BIN) $(LIBS)

run-gles: gles
	./$(GLES_BIN)

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
	  -Ithird_party -Ithird_party/lua -Ithird_party/miniz -Isrc -I$(SDL3_G28_PREFIX)/include -isystem /usr/include \
	  src/*.c third_party/lua/lua.c third_party/miniz/miniz.c third_party/miniz/miniz_zip.c \
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
		-Isrc -Ithird_party -Ithird_party/lua -Ithird_party/miniz -I$(SDL3WIN_INC) \
		$(SRC) -o build/firerender.exe \
		$(SDL3WIN_LIB) \
		-lopengl32 -lwinmm -luser32 -lgdi32 -limm32 -lole32 -loleaut32 \
		-lshell32 -lversion -luuid -lsetupapi -lws2_32 -ldwmapi -lbcrypt \
		-static -static-libgcc

# Пакет для Windows: exe + папки данных рядом (map/, img/, shaders/, lua/, audio/).
# Вшитых данных в движке нет — ресурсы ищутся в pak-zip карты, затем на диске
# (см. resources.c: pak -> диск -> base_dir -> ../).
winpkg: win
	rm -rf build/win
	mkdir -p build/win
	cp build/firerender.exe build/win/
	@echo "Готово: build/win/firerender.exe — нужны папки ресурсов рядом или выше"

# Пересобрать карту в maps/<имя>.zip (по умолчанию map1): map.json, map.png,
# все ресурсы карты. Карту выбирают в меню или командой map <имя>.
mapzip:
	$(PYTHON) tools/make_map_zip.py $(MAP)

# Запечь свет карты офлайн (map-компиляция): считает освещение в 3D со всеми
# переотражениями и AO, пишет map/baked/lightmap.png + probes.bin, затем
# перепаковывает zip карты. Движок в рантайме только читает (cvar r_baked_light).
# Требует собранный $(BIN). Прогон headless (FR_TEST=S грузит мир).
mapbake: $(BIN)
	FR_BAKE=1 FR_TEST=S SDL_VIDEODRIVER=$${SDL_VIDEODRIVER:-dummy} ./$(BIN) || true
	$(PYTHON) tools/make_map_zip.py $(MAP)
	@echo "Свет карты запечён и упакован в maps/$(if $(MAP),$(MAP),map1).zip"

# Запечь свет+тени для ВСЕХ карт maps/*.zip. Для каждой карты движок грузит
# именно её (FR_MAP=<имя>), считает лайтмап+sunvis+пробы в map/baked/, затем
# tools/inject_baked.py кладёт свежий baked/* обратно в её zip (геометрию карты
# не трогает — в отличие от make_map_zip.py, который пересобирает из папки map/).
MAPS := $(patsubst maps/%.zip,%,$(wildcard maps/*.zip))
BAKE_STAMP := build/.baked.stamp

# Общий цикл запекания: для каждой карты — движок с FR_MAP + вкладка baked/*.
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

# Стамп: сборка движка тянет за собой пересборку теней всех карт (см. цель all).
# $(BIN) в рамках одного «make» собирается ровно раз — второй сборки/инкремента
# номера здесь нет (цикл вызывает уже готовый бинарник, не пересобирает).
$(BAKE_STAMP): $(BIN)
	$(BAKE_ALL_MAPS)
	@mkdir -p build && touch $@

# Цель по умолчанию: собрать движок И пересобрать тени всех карт (BAKE=1).
# Быстрая итерация без запекания:  make BAKE=0   или   make $(BIN)
BAKE ?= 1
.PHONY: all
all: $(BIN)
ifeq ($(BAKE),1)
all: $(BAKE_STAMP)
endif


clean:
	rm -f $(BIN)

# Полная очистка: удаляет и бинарник, и build/ целиком.
distclean:
	rm -rf build

# --- Android APK (одной командой; см. tools/make_apk.sh) ----------------------
# Нужны: Android SDK (/opt/android-sdk по умолчанию или ANDROID_SDK=...), NDK
# r28+, build-tools 36. Ресурсы пакуются в APK как assets и распаковываются
# на устройстве при первом запуске. ABI: arm64-v8a + x86_64 (эмулятор);
# сузить: FR_ABIS="arm64-v8a" make apk
apk:
	ANDROID_SDK=$(if $(ANDROID_SDK),$(ANDROID_SDK),/opt/android-sdk) tools/make_apk.sh
	@echo "Готово: build/android/firerender.apk"

# .PHONY — эти цели не являются файлами, их нужно выполнять всегда.
.PHONY: run gles run-gles clean distclean linux win winpkg apk buildnum

# --- Статус рендерных бэкендов (информационно) --------------------------------
# Движок мульти-API по архитектуре: весь GPU-код идёт через sr_* (srender.h).
# Сегодня: OpenGL (make) и GLES (make gles). Vulkan/Metal — зачатки в
# src/srender_stub.c; включаются реализацией srender_vk.c / srender_metal.c
# и флагами -DSR_ENABLE_VULKAN=1 / -DSR_ENABLE_METAL=1.
#   Vulkan: шейдеры GLSL -> SPIR-V (glslc --target-env=vulkan1.1),
#           поверхность SDL_Vulkan_CreateSurface.
#   Metal:  MoltenVK (SPIR-V как есть) или SPIRV-Cross -> MSL + SDL_Metal_CreateView.
backends:
	@echo "Готовые бэкенды:      OpenGL (make), GLES 3.x (make gles)"
	@echo "Заготовки (stubs):    Vulkan, Metal, Null — src/srender_stub.c"
	@echo "Путь включения:       srender_vk.c / srender_metal.c + -DSR_ENABLE_*=1"
	@echo "Статус в игре:        лог старта 'Render API: ...' (sr_api_report)"

# --- Экспериментальные бэкенды (make vulkan / make metal) ---------------------
# Каркас: sr_create() поднимает Vulkan device (srender_vk.c) / Metal-каркас
# (srender_metal.c, только __APPLE__); srender_gl.c исключается. Рендер-
# пайплайн на эти API ещё не портирован — бинарник поднимает девайс и
# завершается с понятным логом. macOS-цель собирать только на Apple.
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
