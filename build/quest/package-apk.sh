#!/bin/sh
# Packages libpyrogenesis.so and its dependencies into an APK without Gradle.
# Output: build/quest/out/zeroad-quest.apk
set -e

cd "$(dirname "$0")"
QUEST_DIR="$(pwd)"
ROOT="$QUEST_DIR/../.."

: "${ANDROID_SDK:=$HOME/Android/Sdk}"
: "${NDK:=$ANDROID_SDK/ndk/26.2.11394342}"
: "${BUILD_TOOLS:=$ANDROID_SDK/build-tools/35.0.0}"
: "${PLATFORM:=$ANDROID_SDK/platforms/android-35/android.jar}"
: "${KEYSTORE:=$HOME/.android/debug.keystore}"

PREFIX="$QUEST_DIR/prefix"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
OUT="$QUEST_DIR/out"
STAGE="$OUT/stage"

rm -rf "$STAGE"
mkdir -p "$STAGE/classes" "$STAGE/dex" "$STAGE/apk/lib/arm64-v8a"

# Java: SDL's activity glue plus our subclass.
find "$PREFIX/share/sdl-java" "$QUEST_DIR/app/java" -name "*.java" >"$STAGE/sources.txt"
javac -nowarn -source 1.8 -target 1.8 -Xlint:-options -encoding UTF-8 \
	-classpath "$PLATFORM" -d "$STAGE/classes" @"$STAGE/sources.txt"
"$BUILD_TOOLS/d8" --release --min-api 29 --lib "$PLATFORM" --output "$STAGE/dex" \
	$(find "$STAGE/classes" -name "*.class")

# Resources and manifest.
"$BUILD_TOOLS/aapt2" compile --dir "$QUEST_DIR/app/res" -o "$STAGE/res.zip"
"$BUILD_TOOLS/aapt2" link -o "$STAGE/base.apk" -I "$PLATFORM" \
	--manifest "$QUEST_DIR/app/AndroidManifest.xml" "$STAGE/res.zip"

# Native libraries, stripped.
for lib in \
	"$ROOT/binaries/system/libpyrogenesis.so" \
	"$PREFIX/lib/libmozjs128-release.so" \
	"$PREFIX/lib/libSDL2.so" \
	"$PREFIX/lib/libopenal.so" \
	"$PREFIX/lib/libopenxr_loader.so" \
	"$TOOLCHAIN/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"; do
	"$TOOLCHAIN/bin/llvm-strip" --strip-unneeded -o "$STAGE/apk/lib/arm64-v8a/$(basename "$lib")" "$lib"
done

cp "$STAGE/dex/classes.dex" "$STAGE/apk/"
cp "$STAGE/base.apk" "$STAGE/unaligned.apk"
(cd "$STAGE/apk" && zip -qr "$STAGE/unaligned.apk" classes.dex lib)

"$BUILD_TOOLS/zipalign" -f -p 4 "$STAGE/unaligned.apk" "$STAGE/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android \
	--out "$OUT/zeroad-vr.apk" "$STAGE/aligned.apk"

ls -la "$OUT/zeroad-vr.apk"
