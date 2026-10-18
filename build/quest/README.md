# 0 A.D. VR for Meta Quest (standalone)

Cross-compiles pyrogenesis for Meta Quest 3/3S (arm64-v8a Android, Vulkan +
OpenXR). The battlefield is shown as a 3D tabletop, the GUI on a floating
panel; VR and mixed reality (passthrough) modes. VR code: `source/ps/QuestVR.cpp`.

## Controls

Right controller: laser = mouse, trigger = left click / drag to box select,
grip = right click (orders), A = follow selection, B = Escape,
stick = rotate (left/right) and zoom (up/down).

Left controller: stick = scroll, trigger = Shift (batch train, add to
selection), grip = Ctrl, X = idle worker, Y = pause, Menu = game menu,
short stick click = switch VR / mixed reality.

Both stick clicks held 1 s = recenter. A virtual keyboard appears for text fields.

## Requirements

- Android SDK in `~/Android/Sdk` with NDK r26c (`ndk;26.2.11394342`, required by
  SpiderMonkey), build-tools 35 and platform android-35
- Rust (rustup) with the `aarch64-linux-android` target, and `cbindgen`
- JDK 17, CMake, Ninja, Python 3 with PyYAML

## Steps

    ./build-spidermonkey.sh   # mozjs 128 -> prefix/
    ./build-deps.sh           # SDL2, ICU, libxml2, curl, OpenAL, OpenXR loader, ... -> prefix/
    ./build-host-tools.sh     # desktop pyrogenesis for data conversion
    ./build-shaders.sh        # GLSL -> SPIR-V for the Vulkan backend
    ./build-engine.sh         # binaries/system/libpyrogenesis.so
    ./package-apk.sh          # out/zeroad-vr.apk
    ./stage-data.sh           # converted data -> stage/0AD/data
    ./stage-data.sh push      # -> /sdcard/0AD/data on the headset
    ./quest-run.sh 60 -- -autostart=skirmishes/acropolis_bay_2p   # test run

## Game data

The APK contains no data and no Collada/NVTT converters, so the mods are
pre-converted on desktop (`stage-data.sh`). The game reads its data from
`/sdcard/0AD/data` and needs the "All files access" permission. Quest
settings (resolution, effects) are written into the staged `default.cfg` by
`quest-config.py`; the desktop config stays untouched.
