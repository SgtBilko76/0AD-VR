#!/bin/sh
# Cross-compiles the third-party libraries needed by pyrogenesis for
# Meta Quest (arm64-v8a Android). Output: build/quest/prefix
#
# Usage: build-deps.sh [lib ...]   (default: all)
set -e

cd "$(dirname "$0")"
QUEST_DIR="$(pwd)"

: "${ANDROID_SDK:=$HOME/Android/Sdk}"
: "${NDK:=$ANDROID_SDK/ndk/26.2.11394342}"
: "${ANDROID_API:=29}"
: "${JOBS:=$(nproc)}"

PREFIX="$QUEST_DIR/prefix"
WORK="$QUEST_DIR/work"
SRC="$WORK/src"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
TARGET=aarch64-linux-android
TRIPLE="$TARGET$ANDROID_API"

mkdir -p "$PREFIX/lib/pkgconfig" "$PREFIX/include" "$SRC"

export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
unset PKG_CONFIG_PATH

SDL_V=2.30.12
BOOST_V=1.86.0
FMT_V=11.1.4
PNG_V=1.6.47
FREETYPE_V=2.13.3
XML2_V=2.13.6
ICU_V=76.1
CURL_V=8.12.1
SODIUM_V=1.0.20
ENET_V=1.3.18
OPENAL_V=1.24.2
OGG_V=1.3.5
VORBIS_V=1.3.7
OPENXR_V=1.1.63

fetch()
{
	# fetch <url> <file>
	if [ ! -e "$SRC/$2" ]; then
		curl -fL --retry 3 -o "$SRC/$2.part" "$1"
		mv "$SRC/$2.part" "$SRC/$2"
	fi
}

unpack()
{
	# unpack <file> <dir>
	rm -rf "${WORK:?}/$2"
	tar xf "$SRC/$1" -C "$WORK"
}

cmake_build()
{
	# cmake_build <srcdir> [cmake args...]
	dir="$1"
	shift
	cmake -S "$dir" -B "$dir/build-android" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
		-DANDROID_ABI=arm64-v8a \
		-DANDROID_PLATFORM="android-$ANDROID_API" \
		-DANDROID_STL=c++_shared \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_FIND_ROOT_PATH="$PREFIX" \
		-DCMAKE_PREFIX_PATH="$PREFIX" \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		"$@"
	cmake --build "$dir/build-android" -j "$JOBS"
	cmake --install "$dir/build-android"
}

autotools_env()
{
	export CC="$TOOLCHAIN/bin/$TRIPLE-clang"
	export CXX="$TOOLCHAIN/bin/$TRIPLE-clang++"
	export AR="$TOOLCHAIN/bin/llvm-ar"
	export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
	export STRIP="$TOOLCHAIN/bin/llvm-strip"
	export CFLAGS="-O2 -fPIC"
	export CXXFLAGS="-O2 -fPIC"
	export CPPFLAGS="-I$PREFIX/include"
	export LDFLAGS="-L$PREFIX/lib"
}

build_zlib()
{
	# zlib ships with the NDK sysroot; only provide a pkg-config file.
	cat >"$PREFIX/lib/pkgconfig/zlib.pc" <<EOF
Name: zlib
Description: zlib from the Android NDK sysroot
Version: 1.3
Libs: -lz
Cflags:
EOF
}

build_sdl()
{
	fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_V/SDL2-$SDL_V.tar.gz" "SDL2-$SDL_V.tar.gz"
	unpack "SDL2-$SDL_V.tar.gz" "SDL2-$SDL_V"
	cmake_build "$WORK/SDL2-$SDL_V" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
	# The Java glue for the SDL activity is needed by the APK project.
	rm -rf "$PREFIX/share/sdl-java"
	mkdir -p "$PREFIX/share/sdl-java"
	cp -R "$WORK/SDL2-$SDL_V/android-project/app/src/main/java/org" "$PREFIX/share/sdl-java/"
}

build_boost()
{
	# Only headers are needed.
	v_=$(echo "$BOOST_V" | tr . _)
	fetch "https://archives.boost.io/release/$BOOST_V/source/boost_$v_.tar.bz2" "boost_$v_.tar.bz2"
	unpack "boost_$v_.tar.bz2" "boost_$v_"
	rm -rf "$PREFIX/include/boost"
	cp -R "$WORK/boost_$v_/boost" "$PREFIX/include/"
	rm -rf "${WORK:?}/boost_$v_"
}

build_fmt()
{
	fetch "https://github.com/fmtlib/fmt/releases/download/$FMT_V/fmt-$FMT_V.zip" "fmt-$FMT_V.zip"
	rm -rf "$WORK/fmt-$FMT_V"
	unzip -q "$SRC/fmt-$FMT_V.zip" -d "$WORK"
	cmake_build "$WORK/fmt-$FMT_V" -DFMT_DOC=OFF -DFMT_TEST=OFF -DBUILD_SHARED_LIBS=OFF
}

build_png()
{
	fetch "https://download.sourceforge.net/libpng/libpng-$PNG_V.tar.xz" "libpng-$PNG_V.tar.xz"
	unpack "libpng-$PNG_V.tar.xz" "libpng-$PNG_V"
	cmake_build "$WORK/libpng-$PNG_V" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
}

build_freetype()
{
	fetch "https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE_V.tar.xz" "freetype-$FREETYPE_V.tar.xz"
	unpack "freetype-$FREETYPE_V.tar.xz" "freetype-$FREETYPE_V"
	cmake_build "$WORK/freetype-$FREETYPE_V" -DBUILD_SHARED_LIBS=OFF \
		-DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
		-DFT_REQUIRE_PNG=ON -DFT_REQUIRE_ZLIB=ON
}

build_libxml2()
{
	fetch "https://download.gnome.org/sources/libxml2/2.13/libxml2-$XML2_V.tar.xz" "libxml2-$XML2_V.tar.xz"
	unpack "libxml2-$XML2_V.tar.xz" "libxml2-$XML2_V"
	cmake_build "$WORK/libxml2-$XML2_V" -DBUILD_SHARED_LIBS=OFF \
		-DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ICU=OFF \
		-DLIBXML2_WITH_ICONV=ON -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_PROGRAMS=OFF \
		-DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_FTP=OFF
}

build_icu()
{
	v_=$(echo "$ICU_V" | tr . _)
	v__=$(echo "$ICU_V" | tr . -)
	fetch "https://github.com/unicode-org/icu/releases/download/release-$v__/icu4c-$v_-src.tgz" "icu4c-$v_-src.tgz"
	unpack "icu4c-$v_-src.tgz" icu
	# ICU cross-builds need a native build of its tools first.
	(
		mkdir -p "$WORK/icu/build-host"
		cd "$WORK/icu/build-host"
		../source/configure --disable-tests --disable-samples --disable-extras >/dev/null
		make -j "$JOBS" >/dev/null
	)
	(
		autotools_env
		mkdir -p "$WORK/icu/build-android"
		cd "$WORK/icu/build-android"
		../source/configure --host=$TARGET --prefix="$PREFIX" \
			--with-cross-build="$WORK/icu/build-host" \
			--enable-static --disable-shared --disable-tests --disable-samples \
			--disable-extras --disable-tools --with-data-packaging=static
		make -j "$JOBS"
		make install
	)
}

build_curl()
{
	fetch "https://curl.se/download/curl-$CURL_V.tar.xz" "curl-$CURL_V.tar.xz"
	unpack "curl-$CURL_V.tar.xz" "curl-$CURL_V"
	# TODO: TLS (mbedtls) for mod.io downloads; plain HTTP is enough to link and start.
	cmake_build "$WORK/curl-$CURL_V" -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF \
		-DCURL_USE_OPENSSL=OFF -DCURL_ENABLE_SSL=OFF -DCURL_USE_LIBPSL=OFF \
		-DCURL_USE_LIBSSH2=OFF -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF \
		-DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF \
		-DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF
}

build_sodium()
{
	fetch "https://download.libsodium.org/libsodium/releases/libsodium-$SODIUM_V.tar.gz" "libsodium-$SODIUM_V.tar.gz"
	unpack "libsodium-$SODIUM_V.tar.gz" "libsodium-$SODIUM_V"
	(
		autotools_env
		cd "$WORK/libsodium-$SODIUM_V"
		./configure --host=$TARGET --prefix="$PREFIX" --enable-static --disable-shared \
			--with-pic --disable-ssp
		make -j "$JOBS"
		make install
	)
}

build_enet()
{
	fetch "http://enet.bespin.org/download/enet-$ENET_V.tar.gz" "enet-$ENET_V.tar.gz"
	unpack "enet-$ENET_V.tar.gz" "enet-$ENET_V"
	(
		autotools_env
		cd "$WORK/enet-$ENET_V"
		./configure --host=$TARGET --prefix="$PREFIX" --enable-static --disable-shared --with-pic
		make -j "$JOBS"
		make install
	)
}

build_openal()
{
	fetch "https://github.com/kcat/openal-soft/archive/refs/tags/$OPENAL_V.tar.gz" "openal-soft-$OPENAL_V.tar.gz"
	unpack "openal-soft-$OPENAL_V.tar.gz" "openal-soft-$OPENAL_V"
	cmake_build "$WORK/openal-soft-$OPENAL_V" -DLIBTYPE=SHARED \
		-DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF \
		-DALSOFT_BACKEND_OPENSL=ON -DALSOFT_REQUIRE_OPENSL=ON
}

build_vorbis()
{
	fetch "https://downloads.xiph.org/releases/ogg/libogg-$OGG_V.tar.xz" "libogg-$OGG_V.tar.xz"
	fetch "https://downloads.xiph.org/releases/vorbis/libvorbis-$VORBIS_V.tar.xz" "libvorbis-$VORBIS_V.tar.xz"
	unpack "libogg-$OGG_V.tar.xz" "libogg-$OGG_V"
	unpack "libvorbis-$VORBIS_V.tar.xz" "libvorbis-$VORBIS_V"
	cmake_build "$WORK/libogg-$OGG_V" -DBUILD_SHARED_LIBS=OFF -DINSTALL_DOCS=OFF
	cmake_build "$WORK/libvorbis-$VORBIS_V" -DBUILD_SHARED_LIBS=OFF
	# Static libs: make pkg-config always emit the dependencies.
	sed -i 's/^Requires.private:/Requires:/' "$PREFIX"/lib/pkgconfig/vorbis*.pc
}

build_httplib()
{
	# Reuses the tarball fetched by libraries/source/cpp-httplib/build.sh.
	v=0.30.1
	tarball="$QUEST_DIR/../../libraries/source/cpp-httplib/cpp-httplib-$v.tar.gz"
	[ -e "$tarball" ] || (cd "$QUEST_DIR/../../libraries/source/cpp-httplib" && ./build.sh)
	rm -rf "$WORK/cpp-httplib-$v"
	tar xf "$tarball" -C "$WORK"
	cmake_build "$WORK/cpp-httplib-$v" -DBUILD_SHARED_LIBS=NO -DCMAKE_INSTALL_LIBDIR=lib \
		-DHTTPLIB_COMPILE=YES -DHTTPLIB_REQUIRE_BROTLI=NO -DHTTPLIB_REQUIRE_OPENSSL=NO \
		-DHTTPLIB_REQUIRE_ZLIB=NO -DHTTPLIB_USE_BROTLI_IF_AVAILABLE=NO \
		-DHTTPLIB_USE_OPENSSL_IF_AVAILABLE=NO -DHTTPLIB_USE_ZLIB_IF_AVAILABLE=NO \
		-DHTTPLIB_USE_ZSTD_IF_AVAILABLE=NO
}

build_openxr()
{
	# Khronos OpenXR loader for Android (prefab AAR): headers + libopenxr_loader.so
	fetch "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$OPENXR_V/openxr_loader_for_android-$OPENXR_V.aar" \
		"openxr_loader_for_android-$OPENXR_V.aar"
	rm -rf "$WORK/openxr-aar"
	mkdir -p "$WORK/openxr-aar"
	(cd "$WORK/openxr-aar" && unzip -q "$SRC/openxr_loader_for_android-$OPENXR_V.aar")
	rm -rf "$PREFIX/include/openxr"
	cp -r "$WORK/openxr-aar/prefab/modules/headers/include/openxr" "$PREFIX/include/"
	cp "$WORK/openxr-aar/prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so" "$PREFIX/lib/"
}

ALL="zlib httplib sdl boost fmt png freetype libxml2 icu curl sodium enet openal vorbis openxr"
[ "$#" -gt 0 ] && ALL="$*"

for lib in $ALL; do
	echo "=== Building $lib ==="
	"build_$lib"
done

echo "Done: libraries installed to $PREFIX"
