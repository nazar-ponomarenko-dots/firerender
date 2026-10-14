#!/bin/sh
# ============================================================================
#  make_apk.sh — build firerender.apk with ONE command (make apk)
# ----------------------------------------------------------------------------
#  Fully local, no gradle: NDK cross-compilation of the engine -> aapt2 ->
#  javac+d8 (SDLActivity) -> zipalign -> apksigner (debug key).
#  ABIs: arm64-v8a (phones/tablets) + x86_64 (emulator).
#  Resources (map/, img/, shaders/, lua/, audio/, cubemaps/, maps/)
#  are packed into the APK as assets and unpacked on the device on first
#  launch (src/android.c, manifest list — assets/assets.txt).
#  models/ temporarily excluded (the heavy vaz-2105.glb at 47M triggers ANR).
# ============================================================================
set -e
cd "$(dirname "$0")/.."

SDK=${ANDROID_SDK:-/opt/android-sdk}
NDK=${NDK:-$SDK/ndk/28.2.13676358}
BT=$SDK/build-tools/36.0.0
PLATFORM=$SDK/platforms/android-36/android.jar
CMAKE="$SDK/cmake/3.22.1/bin/cmake"
ENV_CLEAN="env -u LD_PRELOAD -u LD_LIBRARY_PATH"   # the AppImage environment breaks cmake

ABIS=${FR_ABIS:-"arm64-v8a x86_64"}
VERSION=$(git describe --tags --always 2>/dev/null || echo dev)
# take versionCode from buildnum.txt so it matches ENGINE_BUILD_NUM
if [ -f buildnum.txt ]; then BUILD_NUM=$(cat buildnum.txt); else BUILD_NUM=1; fi
BUILD=build/android
PKG=ru.zaworg.firerender

rm -rf "$BUILD"
mkdir -p "$BUILD"/{lib,assets,gen,classes,filtered}

echo "== 1/6 SDL3 (вендорена, third_party/SDL) =="
for ABI in $ABIS; do
    SDLB=build/and-sdl-$ABI
    if [ ! -f "$SDLB/libSDL3.so" ]; then
        $ENV_CLEAN $CMAKE -S third_party/SDL -B "$SDLB" \
            -DBUILD_SHARED_LIBS=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
            -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
            -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-24 \
            > /tmp/apk-sdl-conf-$ABI.log 2>&1
        $ENV_CLEAN $CMAKE --build "$SDLB" -j$(nproc) > /tmp/apk-sdl-build-$ABI.log 2>&1
    fi
    mkdir -p "$BUILD/lib/$ABI"
    cp "$SDLB/libSDL3.so" "$BUILD/lib/$ABI/"
done

echo "== 2/6 движок (libmain.so) =="
# GLES path: __ANDROID__ enables it by itself (gl_loader.h)
SRC="src/*.c third_party/lua/lua.c third_party/miniz/miniz.c third_party/miniz/miniz_zip.c third_party/miniz/miniz_tinfl.c third_party/miniz/miniz_tdef.c"
for ABI in $ABIS; do
    case $ABI in
        arm64-v8a) TRIPLE24=aarch64-linux-android24 ;;
        x86_64)    TRIPLE24=x86_64-linux-android24 ;;
        *) echo "неизвестный ABI $ABI"; exit 1 ;;
    esac
    # NDK r28: only wrapper names carrying the API level (android-24 = our minimum)
    CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/$TRIPLE24-clang
    $CC -O2 -Wall -Wextra -shared -fPIC \
        -Isrc -Ithird_party -Ithird_party/lua -Ithird_party/miniz \
        -Ithird_party/SDL/include \
        $SRC -o "$BUILD/lib/$ABI/libmain.so" \
        -L"$BUILD/lib/$ABI" -lSDL3 -lm -landroid -llog \
        -Wl,--no-undefined > /tmp/apk-main-$ABI.log 2>&1 \
        || { echo "ошибка компиляции $ABI:"; grep -E "error|ошибка" /tmp/apk-main-$ABI.log | head -20; exit 1; }
done

echo "== 3/6 ресурсы в assets =="
# assets.txt — manifest for unpacking (src/android.c); icons are not needed
# models/ excluded — see the comment in the header
printf '%s\n' "$VERSION" > "$BUILD/assets/assets.version"
: > "$BUILD/assets/assets.txt"
for d in map img shaders lua audio cubemaps maps; do
    [ -d "$d" ] || continue
    mkdir -p "$BUILD/assets/$d"
    cp -r "$d"/. "$BUILD/assets/$d/"
done
find map img shaders lua audio cubemaps maps -type f ! -name '.*' 2>/dev/null | sort > "$BUILD/assets/assets.txt"
cp "$BUILD/assets/assets.txt" /tmp/apk-assets-list.txt

echo "== 4/6 java (SDLActivity) =="
mkdir -p "$BUILD/gen"
cp -r third_party/SDL/android-project/app/src/main/java/org "$BUILD/gen/"
mkdir -p "$BUILD/gen/ru/zaworg/firerender"
cp android/app/src/main/java/ru/zaworg/firerender/MainActivity.java "$BUILD/gen/ru/zaworg/firerender/"
find "$BUILD/gen" -name '*.java' > "$BUILD/java-list.txt"
$SDK/../android-studio/jbr/bin/javac --release 11 -cp "$PLATFORM" \
    -d "$BUILD/classes" @"$BUILD/java-list.txt" > /tmp/apk-javac.log 2>&1 \
    || { echo "javac:"; tail -10 /tmp/apk-javac.log; exit 1; }
$BT/d8 --release --min-api 24 --lib "$PLATFORM" \
    $(find "$BUILD/classes" -name '*.class') \
    --output "$BUILD" > /tmp/apk-d8.log 2>&1 || { echo "d8:"; tail -5 /tmp/apk-d8.log; exit 1; }

echo "== 5/6 упаковка (aapt2) =="
# Pass sdk/version explicitly — otherwise aapt2 sets sdkVersion=1 and versionCode='' (see aapt2 dump badging)
$BT/aapt2 link -I "$PLATFORM" \
    --manifest android/app/src/main/AndroidManifest.xml \
    --java "$BUILD/gen" --auto-add-overlay \
    --min-sdk-version 24 --target-sdk-version 36 \
    --version-code "$BUILD_NUM" --version-name "$VERSION" \
    -A "$BUILD/assets" \
    -o "$BUILD/base.apk"

cd "$BUILD"
zip -q -j base.apk classes.dex   # dex into the APK root
for ABI in $ABIS; do
    zip -q base.apk "lib/$ABI/libSDL3.so" "lib/$ABI/libmain.so"
done
# Important: the path inside the APK MUST be lib/<ABI>/ — only that is seen by PackageManager.
# extractNativeLibs="true" in the manifest, so compression is not critical, but you can
# store uncompressed (-0) for extractNativeLibs=false.

echo "== 6/6 подпись (debug) =="
$BT/zipalign -f 4 base.apk aligned.apk
KS=~/.android/debug.keystore
if [ ! -f "$KS" ]; then
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
$BT/apksigner sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --out firerender.apk aligned.apk
$BT/apksigner verify firerender.apk && echo "APK OK: $(pwd)/firerender.apk ($(du -h firerender.apk | cut -f1))"
