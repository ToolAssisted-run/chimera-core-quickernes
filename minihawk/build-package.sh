#!/bin/sh
# Builds the QuickerNES core package for miniHawk and installs quickernes.zip into
# <MiniHawkRoot>/build/Cores/. Shell port of build-package.ps1 for Linux hosts.
# Prereq: the miniHawk solution has been built (this package references the contract
# DLLs and the settings source generator from <MiniHawkRoot>/build/dll), and the
# native has been built (native/Makefile: make && make install -> natives/).
#
# Usage: ./build-package.sh [-c Configuration] [-r MiniHawkRoot]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
configuration=Release
minihawk_root=""
while getopts "c:r:" opt; do
	case "$opt" in
		c) configuration="$OPTARG" ;;
		r) minihawk_root="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
[ -n "$minihawk_root" ] || minihawk_root="$here/../../BizHawk"
minihawk_root="$(cd "$minihawk_root" && pwd)"

dotnet build "$here/MiniHawk.QuickerNES.csproj" -c "$configuration" -p:MiniHawkRoot="$minihawk_root" -v q --nologo

staging="$here/bin/package-staging"
rm -rf "$staging"
mkdir -p "$staging"

cp "$here/minihawk-core.json" "$staging"
cp "$here/bin/$configuration/MiniHawk.QuickerNES.dll" "$staging"
cp "$here/defctrl.json" "$staging"
cp -r "$here/lua" "$staging/lua"
found_native=0
for native in libquicknes.dll libquicknes.so; do
	if [ -f "$here/natives/$native" ]; then
		cp "$here/natives/$native" "$staging"
		found_native=1
	fi
done
[ "$found_native" -eq 1 ] || { echo "no libquicknes native found in $here/natives (run make -C native && make -C native install)" >&2; exit 1; }

cores_dir="$minihawk_root/build/Cores"
mkdir -p "$cores_dir"
zip_path="$cores_dir/quickernes.zip"
rm -f "$zip_path"
# stdlib zip keeps this dependency-free (no zip(1) on minimal hosts)
python3 - "$staging" "$zip_path" <<'EOF'
import os, sys, zipfile
staging, zip_path = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(staging):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(root, name)
            z.write(full, os.path.relpath(full, staging))
EOF
# stale extracted caches are keyed by zip timestamp and cleaned up by the loader
# itself; clear them here too, best-effort, to keep the tree tidy
for cache in "$minihawk_root"/build/CoreCache/quickernes-* "$cores_dir"/_cache/quickernes-*; do
	[ -d "$cache" ] && rm -rf "$cache" || true
done
echo "packaged -> $zip_path"
