#!/bin/sh
# Stages the game data for the Quest in stage/0AD/data (pushed to /sdcard/0AD/data):
#   config/default.cfg  desktop defaults + Quest VR overrides (quest-config.py)
#   l10n/
#   mods/mod, mods/public  pre-converted archives built by the desktop pyrogenesis
#                          (build-host-tools.sh), since the Quest build has no
#                          Collada/NVTT converters.
#
# Usage: stage-data.sh [config] [mod] [public] [push]   (default: all but push)
set -e

cd "$(dirname "$0")"
QUEST_DIR="$(pwd)"
DATA="$QUEST_DIR/../../binaries/data"
STAGE="$QUEST_DIR/stage/0AD/data"
: "${ADB:=$HOME/Android/Sdk/platform-tools/adb}"

STEPS="${*:-config mod public}"

mkdir -p "$STAGE/config" "$STAGE/mods"

for step in $STEPS; do
	case "$step" in
	config)
		python3 "$QUEST_DIR/quest-config.py" "$DATA/config/default.cfg" "$STAGE/config/default.cfg"
		cp "$DATA/config/keys.txt" "$STAGE/config/"
		rm -rf "$STAGE/l10n"
		cp -r "$DATA/l10n" "$STAGE/l10n"
		;;
	mod|public)
		[ -x "$QUEST_DIR/host-pyrogenesis" ] || { echo "Run build-host-tools.sh first" >&2; exit 1; }
		mkdir -p "$STAGE/mods/$step"
		base=""
		[ "$step" = public ] && base="-mod=mod"
		"$QUEST_DIR/host-pyrogenesis" $base \
			-archivebuild="$(cd "$DATA/mods/$step" && pwd)" \
			-archivebuild-output="$STAGE/mods/$step/$step.zip" \
			-archivebuild-compress
		[ ! -f "$DATA/mods/$step/mod.json" ] || cp "$DATA/mods/$step/mod.json" "$STAGE/mods/$step/"
		;;
	push)
		"$ADB" shell mkdir -p /sdcard/0AD/data
		"$ADB" shell rm -rf /sdcard/0AD/data/mods/mod /sdcard/0AD/data/mods/public
		"$ADB" push "$STAGE/." /sdcard/0AD/data/
		;;
	*)
		echo "Unknown step: $step" >&2
		exit 1
		;;
	esac
done
ls -la "$STAGE" "$STAGE/mods"/* 2>/dev/null || true
