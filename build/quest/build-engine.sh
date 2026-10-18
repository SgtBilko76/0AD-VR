#!/bin/sh
# Builds pyrogenesis as libmain.so for Meta Quest (arm64-v8a Android).
# Requires build-spidermonkey.sh and build-deps.sh to have been run.
set -e

cd "$(dirname "$0")"
QUEST_DIR="$(pwd)"

: "${ANDROID_SDK:=$HOME/Android/Sdk}"
: "${NDK:=$ANDROID_SDK/ndk/26.2.11394342}"
: "${ANDROID_API:=29}"
: "${JOBS:=$(nproc)}"
: "${CONFIG:=release}"

PREFIX="$QUEST_DIR/prefix"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
TRIPLE="aarch64-linux-android$ANDROID_API"
OUTPATH="$QUEST_DIR/../workspaces/android"

export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""
unset PKG_CONFIG_PATH

(
	cd ../premake
	../../libraries/source/premake-core/bin/premake5 --file=premake5.lua \
		--outpath="$OUTPATH" \
		--android \
		--with-system-mozjs --with-system-cpp-httplib \
		--without-atlas --without-tests --without-lobby --without-miniupnpc \
		--without-nvtt --without-dap-interface --without-pch \
		gmake
)

make -C "$OUTPATH" -j "$JOBS" config="$CONFIG" \
	CC="$TOOLCHAIN/bin/$TRIPLE-clang" \
	CXX="$TOOLCHAIN/bin/$TRIPLE-clang++" \
	AR="$TOOLCHAIN/bin/llvm-ar" \
	CPPFLAGS="-isystem $PREFIX/include" \
	LDFLAGS="-L$PREFIX/lib" \
	pyrogenesis
