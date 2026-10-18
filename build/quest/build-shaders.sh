#!/bin/sh
# Compiles the GLSL shaders to SPIR-V for the Vulkan backend.
set -e
cd "$(dirname "$0")"
QUEST_DIR="$(pwd)"
ROOT="$QUEST_DIR/../.."
: "${ANDROID_SDK:=$HOME/Android/Sdk}"
: "${NDK:=$ANDROID_SDK/ndk/26.2.11394342}"
export PATH="$NDK/shader-tools/linux-x86_64:$PATH"
export LD_LIBRARY_PATH="$NDK/shader-tools/linux-x86_64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

mkdir -p work
if [ ! -e work/spirv-rules.json ]; then
	curl -fLo work/spirv-rules.json https://svn.wildfiregames.com/nightly-build/trunk/source/tools/spirv/rules.json
fi
(cd "$ROOT/libraries" && ./source/spirv-reflect/build.sh)

MODS="$ROOT/binaries/data/mods"
for m in mod public; do
	mkdir -p "$MODS/$m/shaders/spirv"
	python3 "$ROOT/source/tools/spirv/compile.py" -d "$MODS/mod" "$MODS/$m" work/spirv-rules.json "$MODS/$m"
done
