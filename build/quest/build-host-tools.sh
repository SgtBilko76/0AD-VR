#!/bin/sh
# Builds a desktop (Linux x86_64) pyrogenesis used only to pre-convert game
# data for the Quest (pyrogenesis -archivebuild): the Quest build has no
# Collada/NVTT converters.
#
# Builds in a separate git worktree (default ../../../0ad-host) so the
# desktop objects in binaries/system don't clash with the Android ones.
# Libraries missing on the host (libxml2, ICU, ENet, libsodium) are built
# into host-prefix/ from the tarballs build-deps.sh downloaded.
set -e

cd "$(dirname "$0")"
QUEST_DIR="$(pwd)"
REPO="$(cd ../.. && pwd)"

: "${JOBS:=$(nproc)}"
: "${HOST_TREE:=$(cd "$REPO/.." && pwd)/0ad-host}"

HPREFIX="$QUEST_DIR/host-prefix"
HWORK="$QUEST_DIR/host-work"
SRC="$QUEST_DIR/work/src"

mkdir -p "$HPREFIX" "$HWORK/bin"
export PKG_CONFIG_PATH="$HPREFIX/lib/pkgconfig:$HPREFIX/lib64/pkgconfig"

# FCollada/NVTT sources are fetched with `svn export`; provide a stand-in when
# Subversion isn't installed.
if ! command -v svn >/dev/null 2>&1; then
	cat > "$HWORK/bin/svn" <<EOF
#!/bin/sh
[ "\$1" = export ] || { echo "svn shim: only 'export' is supported" >&2; exit 1; }
exec python3 "$QUEST_DIR/svn-export.py" "\$2" "\$3"
EOF
	chmod +x "$HWORK/bin/svn"
fi
export PATH="$HWORK/bin:$HOME/.cargo/bin:$PATH"

# SpiderMonkey's build wants llvm-objdump; borrow the NDK's.
: "${NDK:=$HOME/Android/Sdk/ndk/26.2.11394342}"
if ! command -v llvm-objdump >/dev/null 2>&1; then
	ln -sf "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-objdump" "$HWORK/bin/llvm-objdump"
fi

unpack()
{
	# unpack <file> <dir>
	[ -d "$HWORK/$2" ] || tar xf "$SRC/$1" -C "$HWORK"
}

build_autotools()
{
	# build_autotools <file> <dir> [configure args]
	f=$1; d=$2; shift 2
	[ -f "$HWORK/$d/.done" ] && return
	unpack "$f" "$d"
	(
		cd "$HWORK/$d"
		./configure --prefix="$HPREFIX" --enable-shared --disable-static "$@"
		make -j "$JOBS"
		make install
		touch .done
	)
}

build_autotools libxml2-2.13.6.tar.xz libxml2-2.13.6 --without-python --without-lzma
build_autotools libsodium-1.0.20.tar.gz libsodium-1.0.20
build_autotools enet-1.3.18.tar.gz enet-1.3.18

if [ ! -f "$HWORK/icu/.done" ]; then
	unpack icu4c-76_1-src.tgz icu
	(
		mkdir -p "$HWORK/icu/build"
		cd "$HWORK/icu/build"
		../source/configure --prefix="$HPREFIX" --disable-tests --disable-samples
		make -j "$JOBS"
		make install
		touch ../.done
	)
fi

# Worktree without the multi-GB game data; that is symlinked from the repo.
if [ ! -d "$HOST_TREE" ]; then
	git -C "$REPO" worktree add --no-checkout --detach "$HOST_TREE" HEAD
	git -C "$HOST_TREE" sparse-checkout set --no-cone '/*' '!/binaries/data/'
	git -C "$HOST_TREE" checkout --detach HEAD
	ln -s "$REPO/binaries/data" "$HOST_TREE/binaries/data"
fi

"$HOST_TREE/libraries/build-source-libs.sh" -j"$JOBS"

# Upstream doesn't link with --without-lobby; reuse the fix from this tree.
cp "$REPO/source/lobby/Globals.cpp" "$HOST_TREE/source/lobby/Globals.cpp"

(
	cd "$HOST_TREE/build/workspaces"
	./update-workspaces.sh \
		--without-atlas --without-tests --without-lobby --without-miniupnpc \
		--without-dap-interface
	make -C gcc -j "$JOBS" config=release pyrogenesis
)

cat > "$QUEST_DIR/host-pyrogenesis" <<EOF
#!/bin/sh
LD_LIBRARY_PATH="$HPREFIX/lib:$HOST_TREE/binaries/system\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}" \\
	exec "$HOST_TREE/binaries/system/pyrogenesis" "\$@"
EOF
chmod +x "$QUEST_DIR/host-pyrogenesis"
echo "Done: $QUEST_DIR/host-pyrogenesis"
