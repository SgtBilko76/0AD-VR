#!/bin/sh
# Headless test round on a connected Quest: install the APK, wake the headset,
# launch, record the log and take a screenshot.
# Usage: quest-run.sh [seconds] [--no-install] [-- extra game args]
set -e

cd "$(dirname "$0")"
: "${ADB:=$HOME/Android/Sdk/platform-tools/adb}"
: "${OUT_DIR:=$(pwd)/out/run}"
PKG=com.bilko.zeroadvr
SECS=40
INSTALL=1
ARGS=""
while [ "$#" -gt 0 ]; do
	case "$1" in
		--no-install) INSTALL=0 ;;
		--) shift; ARGS="$*"; break ;;
		*) SECS="$1" ;;
	esac
	shift
done

mkdir -p "$OUT_DIR"
if [ "$INSTALL" = 1 ]; then
	"$ADB" install -r out/zeroad-vr.apk >/dev/null
	"$ADB" shell appops set "$PKG" MANAGE_EXTERNAL_STORAGE allow
fi

"$ADB" shell setprop debug.oculus.guardian_pause 1
"$ADB" shell am force-stop "$PKG"
"$ADB" shell input keyevent KEYCODE_WAKEUP
"$ADB" shell am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null
"$ADB" shell svc power stayon true
"$ADB" logcat -c
if [ -n "$ARGS" ]; then
	"$ADB" shell am start -n "$PKG/com.wildfiregames.zeroad.ZeroADActivity" --es args "'$ARGS'" >/dev/null
else
	"$ADB" shell am start -n "$PKG/com.wildfiregames.zeroad.ZeroADActivity" >/dev/null
fi
timeout "$SECS" "$ADB" logcat -v time > "$OUT_DIR/run.log" || true

"$ADB" shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT >/dev/null
sleep 4
SHOT=$("$ADB" shell ls -t /sdcard/Oculus/Screenshots | head -1 | tr -d '\r')
"$ADB" pull "/sdcard/Oculus/Screenshots/$SHOT" "$OUT_DIR/shot.jpg" >/dev/null
echo "screenshot: $SHOT -> $OUT_DIR/shot.jpg"
PID=$("$ADB" shell pidof "$PKG" | tr -d '\r')
echo "pid: ${PID:-not running}"
grep -a -E "QuestVR|pyrogenesis|F DEBUG|AndroidRuntime" "$OUT_DIR/run.log" | grep -v 'NVTT' | tail -40
[ -n "$PID" ] && grep -a "VrApi.*($PID).*FPS" "$OUT_DIR/run.log" | tail -2 | sed 's/,Tear.*CPU4/ ... CPU4/;s/,LCnt.*//'
true
