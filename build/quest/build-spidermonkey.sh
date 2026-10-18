#!/bin/sh
# Cross-compiles SpiderMonkey (mozjs) for Meta Quest (arm64-v8a Android).
# Output: build/quest/prefix/{include/mozjs,lib}
set -e

cd "$(dirname "$0")"
QUEST_DIR="$(pwd)"

: "${ANDROID_SDK:=$HOME/Android/Sdk}"
: "${NDK:=$ANDROID_SDK/ndk/26.2.11394342}"
: "${ANDROID_API:=29}"
: "${JOBS:=-j$(nproc)}"
export PATH="$HOME/.cargo/bin:$PATH"

PV=128.13.0
FOLDER="mozjs-${PV}"
PREFIX="$QUEST_DIR/prefix"
WORK="$QUEST_DIR/work"

mkdir -p "$WORK" "$PREFIX/lib" "$PREFIX/include"
cd "$WORK"

if [ ! -e "${FOLDER}.tar.xz" ]; then
	curl -fLo "${FOLDER}.tar.xz" "https://releases.wildfiregames.com/libs/${FOLDER}.tar.xz"
fi

if [ ! -d "$FOLDER" ]; then
	ln -sfn "$QUEST_DIR/../../libraries/source/spidermonkey/patches" patches
	tar xfJ "${FOLDER}.tar.xz" \
		--exclude=js/src/tests/non262 \
		--exclude=js/src/tests/test262 \
		--exclude=js/src/jit-test \
		--exclude=python/mozperftest \
		--exclude=testing/web-platform
	(cd "$FOLDER" && . ../patches/patch.sh)
fi

cd "$FOLDER"

export MOZ_NOSPAM=1
export CFLAGS="${CFLAGS:-} -w"
export CXXFLAGS="${CXXFLAGS:-} -w"
export MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE="none"
export MOZBUILD_STATE_PATH="$WORK/mozbuild-state"

cat >"$WORK/mozconfig-android" <<EOF
ac_add_options --enable-project=js
ac_add_options --enable-shared-js
ac_add_options --disable-jemalloc
ac_add_options --without-intl-api
ac_add_options --disable-js-shell
ac_add_options --disable-tests
ac_add_options --enable-optimize
# pyrogenesis is built with RTTI on Android (see premake5.lua), so mozjs must provide typeinfos.
ac_add_options --enable-cpp-rtti
ac_add_options --target=aarch64-linux-android
ac_add_options --with-android-ndk=$NDK
ac_add_options --with-android-version=$ANDROID_API
export CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
export CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++"
export HOST_CC=gcc
export HOST_CXX=g++
# mozjs links libc++ statically; keep those symbols private so they do not
# interpose on libc++_shared.so used by pyrogenesis (breaks exception matching).
export LDFLAGS="-Wl,--exclude-libs,libc++_static.a -Wl,--exclude-libs,libc++abi.a -Wl,--exclude-libs,libunwind.a"
mk_add_options MOZ_OBJDIR=@TOPSRCDIR@/build-android
EOF

# shellcheck disable=SC2086
MOZCONFIG="$WORK/mozconfig-android" ./mach build $JOBS

rm -rf "$PREFIX/include/mozjs"
mkdir -p "$PREFIX/include/mozjs"
cp -R -L build-android/dist/include/* "$PREFIX/include/mozjs/"
cp -L build-android/dist/bin/libmozjs128-release.so "$PREFIX/lib/" 2>/dev/null \
	|| cp -L build-android/dist/bin/libmozjs*.so "$PREFIX/lib/"
rust_path="build-android/$(grep jsrust build-android/js/src/build/backend.mk | cut -d / -f 2-)"
cp -L "$rust_path" "$PREFIX/lib/libmozjs128-rust.a"

echo "SpiderMonkey for Android installed to $PREFIX"

cat >"$PREFIX/lib/pkgconfig/mozjs-128.pc" <<PC
prefix=$PREFIX
Name: SpiderMonkey 128
Description: SpiderMonkey for Android arm64
Version: $PV
Libs: -L\${prefix}/lib -lmozjs128-release -lmozjs128-rust
Cflags: -I\${prefix}/include/mozjs
PC
