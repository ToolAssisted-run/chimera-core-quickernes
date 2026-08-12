#!/bin/bash
# Generates the witness movies: one .tas per test, converted from its .sol input
# sequence by sol2tas, which builds the mnemonics from the core's own
# ControllerDefinition.
#
# The free-set movies are committed, so CI needs nothing. Regenerate when the
# controller declaration in waterbox.config changes - the goldens will tell you
# loudly if you forget, since every movie desyncs at once.
#
# Usage:
#   ./make-movies.sh                        # free-to-distribute set (default)
#   ./make-movies.sh --set full             # every test whose rom is available
#   ./make-movies.sh --minihawk-root <path>
set -u

set_name=free
minihawk_root=""
package_dir=""
while [ $# -gt 0 ]; do
	case "$1" in
		--set) set_name="$2"; shift ;;
		--minihawk-root) minihawk_root="$2"; shift ;;
		--package) package_dir="$2"; shift ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done

here="$(cd "$(dirname "$0")" && pwd)"
tests_dir="$(cd "$here/.." && pwd)"
repo_root="$(cd "$tests_dir/../.." && pwd)"
suite="$tests_dir/suite"
out_dir="$tests_dir/movies"
mkdir -p "$out_dir"

if [ -z "$minihawk_root" ]; then
	for candidate in "$repo_root/../miniHawk" "$repo_root/../BizHawk" "$HOME/miniHawk"; do
		[ -d "$candidate" ] && { minihawk_root="$candidate"; break; }
	done
fi
[ -n "$minihawk_root" ] && [ -d "$minihawk_root" ] || { echo "miniHawk checkout not found; pass --minihawk-root" >&2; exit 1; }
minihawk_root="$(cd "$minihawk_root" && pwd)"
dll="$minihawk_root/build/dll"

# the built package: core.wbx + waterbox.config, which is where the controller
# declaration the movies are keyed to comes from
[ -n "$package_dir" ] || package_dir="$repo_root/waterbox/bin"
[ -f "$package_dir/core.wbx" ] || { echo "package not built: $package_dir/core.wbx (run waterbox/build-core.sh)" >&2; exit 1; }

# free set: roms that are free to distribute and vendored in suite/roms
free_set=(
	"sprilo.anyPercent"
	"novaTheSquirrel.anyPercent"
)

converter="$here/sol2tas.exe"
if [ ! -f "$converter" ] || [ "$here/sol2tas.cs" -nt "$converter" ]; then
	echo "building sol2tas..."
	mcs -langversion:latest -out:"$converter" "$here/sol2tas.cs" \
		-r:"$dll/BizHawk.Emulation.Common.dll" -r:"$dll/BizHawk.Client.Common.dll" \
		-r:"$dll/BizHawk.Common.dll" -r:"$dll/BizHawk.BizInvoke.dll" -r:"$dll/Newtonsoft.Json.dll" \
		-r:System.IO.Compression.dll -r:System.IO.Compression.FileSystem.dll || exit 1
fi

roms_dirs=("$suite/roms" "$HOME/TAS/roms/nes")
made=0
skipped=0
for test_file in "$suite"/*.test; do
	name="$(basename "$test_file" .test)"
	if [ "$set_name" = "free" ]; then
		in_free=0
		for f in "${free_set[@]}"; do [ "$name" = "$f" ] && in_free=1; done
		[ "$in_free" -eq 1 ] || continue
	fi

	rom_file="$(python3 -c "
import json, io, sys
t = json.load(io.open('$test_file', encoding='utf-8-sig'))
print(t.get('Rom File', ''))
")"
	rom_path=""
	for dir in "${roms_dirs[@]}"; do
		[ -f "$dir/${rom_file#roms/}" ] && { rom_path="$dir/${rom_file#roms/}"; break; }
	done
	if [ -z "$rom_path" ]; then
		printf "  %-36s SKIP (rom not found: %s)\n" "$name" "$rom_file"
		skipped=$((skipped + 1))
		continue
	fi

	# sol2tas loads the core to read its controller definition, so it needs the
	# host library on the search path
	if LD_LIBRARY_PATH="$dll" MONO_PATH="$dll" mono "$converter" \
		"$package_dir" "$rom_path" "$test_file" "$suite/$name.sol" "$out_dir/$name.tas" > /dev/null 2>&1; then
		printf "  %-36s ok\n" "$name"
		made=$((made + 1))
	else
		printf "  %-36s FAILED\n" "$name"
		skipped=$((skipped + 1))
	fi
done

echo ""
echo "$made movies written to $out_dir, $skipped skipped"
[ "$made" -eq 0 ] && exit 1
exit 0
