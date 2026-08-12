#!/bin/sh
# Builds the quickerNES waterbox core package and installs it into a miniHawk
# checkout as build/Cores/quickernes.zip.
#
# A package is exactly two files - core.wbx (fixed name) plus waterbox.config -
# and miniHawk loads it through its one built-in generic adapter. There is no
# managed assembly and no native library in a package any more.
#
# Usage: ./build-package.sh [-m <miniBox dir>] [-r <miniHawk root>] [-o <build dir>]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
mb="${MINIBOX_DIR:-}"
out="$here/bin"
minihawk_root=""
while getopts "m:r:o:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		r) minihawk_root="$OPTARG" ;;
		o) out="$OPTARG" ;;
		*) exit 2 ;;
	esac
done

if [ -z "$minihawk_root" ]; then
	for candidate in "$here/../../miniHawk" "$here/../../BizHawk" "$HOME/miniHawk"; do
		[ -d "$candidate" ] && { minihawk_root="$candidate"; break; }
	done
fi
[ -n "$minihawk_root" ] && [ -d "$minihawk_root" ] || {
	echo "miniHawk checkout not found; pass -r <path>" >&2; exit 1; }
minihawk_root="$(cd "$minihawk_root" && pwd)"

# miniBox lives in the miniHawk checkout as a submodule; the guest toolchain
# (musl + libstdc++ for the C++ guest) is part of its meson graph.
[ -n "$mb" ] || mb="$minihawk_root/extern/miniBox"
mbuild="$mb/build/meson-cpp"
[ -f "$mbuild/build.ninja" ] || meson setup "$mbuild" "$mb" -Dguest_cpp=true
ninja -C "$mbuild"

sh "$here/build-core.sh" -m "$mb" -o "$out"

staging="$out/package-staging"
rm -rf "$staging"
mkdir -p "$staging"
cp "$out/core.wbx" "$staging/core.wbx"
cp "$here/waterbox.config" "$staging/waterbox.config"

cores_dir="$minihawk_root/build/Cores"
mkdir -p "$cores_dir"
zip_path="$cores_dir/quickernes.zip"
rm -f "$zip_path"
python3 - "$staging" "$zip_path" <<'PYEOF'
import os, sys, zipfile
staging, zip_path = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(staging):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(root, name)
            z.write(full, os.path.relpath(full, staging))
PYEOF

# the frontend extracts packages into build/CoreCache keyed by content; drop any
# stale extraction so the next load picks this build up
for cache in "$minihawk_root"/build/CoreCache/quickernes-*; do
	[ -d "$cache" ] && rm -rf "$cache" || true
done
echo "packaged -> $zip_path"
