#!/usr/bin/env python3
"""Minimal `svn export URL@REV DEST` over HTTP for hosts without Subversion.

Walks mod_dav_svn's HTML directory listings under !svn/bc/<rev>/ and downloads
every file. Only used to fetch FCollada/NVTT sources for the desktop tool build.

Usage: svn-export.py https://svn.wildfiregames.com/public/source-libs/trunk/nvtt@28209 nvtt-28209
"""
import concurrent.futures
import os
import re
import sys
import urllib.parse
import urllib.request

REPO_ROOT = "https://svn.wildfiregames.com/public/"


def fetch(url):
	for attempt in range(4):
		try:
			with urllib.request.urlopen(url, timeout=60) as r:
				return r.read()
		except Exception:
			if attempt == 3:
				raise


def walk(base, rel, files):
	html = fetch(base + rel).decode("utf-8", "replace")
	for href in re.findall(r'<li><a href="([^"]+)">', html):
		if href == "../":
			continue
		if href.endswith("/"):
			walk(base, rel + href, files)
		else:
			files.append(rel + href)


def main():
	src, dest = sys.argv[1], sys.argv[2]
	url, rev = src.rsplit("@", 1)
	path = url[len(REPO_ROOT):].strip("/")
	base = f"{REPO_ROOT}!svn/bc/{rev}/{path}/"
	files = []
	walk(base, "", files)

	def get(rel):
		out = os.path.join(dest, urllib.parse.unquote(rel))
		os.makedirs(os.path.dirname(out), exist_ok=True)
		with open(out, "wb") as f:
			f.write(fetch(base + rel))

	with concurrent.futures.ThreadPoolExecutor(16) as pool:
		list(pool.map(get, files))
	# svn keeps the executable bit; build scripts need it.
	for rel in files:
		if rel.endswith(".sh"):
			os.chmod(os.path.join(dest, urllib.parse.unquote(rel)), 0o755)
	print(f"exported {len(files)} files to {dest}")


if __name__ == "__main__":
	main()
