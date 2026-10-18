#!/usr/bin/env python3
"""Writes the Quest VR defaults into a staged copy of binaries/data/config/default.cfg.

Usage: quest-config.py <source default.cfg> <destination default.cfg>

The desktop defaults stay untouched; only the staged copy for the headset is
changed. Keys are replaced in their [section]; missing keys are appended to it.
"""
import re
import sys

OVERRIDES = {
	"": {
		# Rendering: the scene is drawn twice per frame (one per eye).
		"rendererbackend": '"vulkan"',
		"windowed": "false",
		"vsync": "false",
		"pauseonfocusloss": "false",
		"shadows": "true",
		"shadowquality": "-1",
		"shadowscutoffdistance": "400.0",
		"watereffects": "false",
		"waterfancyeffects": "false",
		"waterrealdepth": "false",
		"waterreflection": "false",
		"waterrefraction": "false",
		"postproc": "false",
		"fog": "false",
		"showsky": "true",
		"silhouettes": "true",
		"particles": "true",
		"cursorbackend": '"system"',
		"renderer.scale": "1.0",
		"materialmgr.quality": "5.0",
		"max_actor_quality": "100",
		# Quest VR mode (source/ps/QuestVR.cpp).
		"quest.vr": "true",
		"quest.eyescale": "1.0",
		"quest.eye.maxwidth": "2064",
		"quest.eye.width": "1840",
		"quest.eye.dynamic": "false",
		"quest.refreshrate": "72",
		"quest.passthrough": "true",
		"quest.passthrough.ingame": "false",
		"quest.panel.width": "1440",
		"quest.panel.height": "900",
	},
	"adaptivefps": {
		# The OpenXR runtime paces frames.
		"session": "0",
		"menu": "0",
	},
	"hotkey.camera": {
		# A button on the right controller.
		"follow": "F13",
	},
	"view": {
		# The tabletop shows the area under a top-down camera.
		"scroll.mouse.detectdistance": "0",
		"rotate.x.min": "90.0",
		"rotate.x.max": "90.0",
		"rotate.x.default": "90.0",
		"fov": "60.0",
		"zoom.min": "60.0",
		"zoom.max": "600.0",
		"zoom.default": "250.0",
		"zoom.speed": "300.0",
		"height.min": "40",
	},
}


def main():
	src, dst = sys.argv[1], sys.argv[2]
	with open(src, encoding="utf-8") as f:
		lines = f.read().split("\n")

	pending = {section: dict(keys) for section, keys in OVERRIDES.items()}
	out = []
	section = ""
	key_re = re.compile(r"^(\s*)([A-Za-z0-9_.]+)(\s*=\s*)([^;]*?)(\s*;.*)?$")

	def flush(section_name):
		for key, value in pending.get(section_name, {}).items():
			out.append(f"{key} = {value} ; Quest VR")
		pending[section_name] = {}

	for line in lines:
		header = re.match(r"^\s*\[([^\]]+)\]", line)
		if header:
			flush(section)
			section = header.group(1).strip()
			out.append(line)
			continue
		m = key_re.match(line)
		if m and m.group(2) in pending.get(section, {}):
			value = pending[section].pop(m.group(2))
			out.append(f"{m.group(1)}{m.group(2)}{m.group(3)}{value} ; Quest VR")
			continue
		out.append(line)
	flush(section)
	for name, keys in pending.items():
		if keys:
			out.append(f"[{name}]")
			flush(name)

	with open(dst, "w", encoding="utf-8") as f:
		f.write("\n".join(out))


if __name__ == "__main__":
	main()
