#!/bin/bash
# The core-level gate for the waterbox port: for every rom given, the sandboxed
# core must produce byte-identical video, audio, lag and memory-domain digests to
# the original shared library, and must survive a whole-machine savestate
# round-trip around every frame.
#
# This is the gate CI runs on every push, over the roms that are free to
# distribute. It needs only gcc, meson and python3 - no .NET, Mono or X - so it
# is the fast half of the witness. The frontend half (movies replayed through
# EmuHawk against goldens) lives in ../minihawk/tests.
#
# Usage:
#   ./run-gate.sh [-n <native build dir>] [-g <guest build dir>] [-f <frames>] [rom...]
# with no roms, it uses the free-to-distribute set vendored in the repo.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
nat="$root/build/meson-native"
gst="$root/build/meson-guest"
frames=600
while getopts "n:g:f:" opt; do
	case "$opt" in
		n) nat="$OPTARG" ;;
		g) gst="$OPTARG" ;;
		f) frames="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))

roms=("$@")
if [ ${#roms[@]} -eq 0 ]; then
	# Free to distribute, vendored in the repo, so this runs anywhere.
	roms=(
		"$root/tests/roms/sprilo.nes"
		"$root/minihawk/tests/suite/roms/nova.nes"   # MMC1 SXROM: 32K banked PRG RAM
	)
fi

native="$nat/libquicknes.so"
[ -f "$native" ] && [ -x "$nat/run-wbx" ] || {
	echo "native build missing: meson setup build/meson-native -Dwaterbox=true && ninja -C build/meson-native" >&2; exit 1; }
[ -f "$gst/core.wbx" ] || {
	echo "guest build missing: sh waterbox/setup-guest.sh && ninja -C build/meson-guest core.wbx" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
digests() { grep -E '^(frames|videoHash|audioHash|lagFrames|domain\[)'; }
# What a turbo run can be held to: everything except the whole-run video hash,
# which a run that skipped the first half cannot possibly match - the second
# half it did draw is compared instead.
turboDigests() { grep -E '^(frames|tailVideoHash|audioHash|lagFrames|domain\[)'; }

ok=0
failed=0
report() { printf "%-28s %-9s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }

printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

for rom in "${roms[@]}"; do
	name="$(basename "$rom" .nes)"
	if [ ! -f "$rom" ]; then report "$name" SKIP "rom not found"; continue; fi

	if ! "$nat/run-native" "$native" "$rom" "$frames" 2>"$work/nat.err" | digests > "$work/nat.txt"; then
		report "$name:equivalence" FAIL "native runner error: $(head -1 "$work/nat.err")"; continue
	fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$rom" "$frames" 2>"$work/box.err" | digests > "$work/box.txt"; then
		report "$name:equivalence" FAIL "waterbox runner error: $(head -1 "$work/box.err")"; continue
	fi
	if cmp -s "$work/nat.txt" "$work/box.txt"; then
		report "$name:equivalence" PASS "$frames frames, native == waterboxed"
	else
		report "$name:equivalence" FAIL "$(diff "$work/nat.txt" "$work/box.txt" | tr '\n' ' ' | head -c 120)"
	fi

	# Round-trip the whole machine through save/load state around every frame:
	# the digests must come out exactly as they do without it.
	if ! "$nat/run-wbx" "$gst/core.wbx" "$rom" "$frames" --rerecord 2>/dev/null | digests > "$work/rr.txt"; then
		report "$name:savestate" FAIL "rerecord runner error"; continue
	fi
	if cmp -s "$work/box.txt" "$work/rr.txt"; then
		report "$name:savestate" PASS "per-frame round-trip is lossless"
	else
		report "$name:savestate" FAIL "$(diff "$work/box.txt" "$work/rr.txt" | tr '\n' ' ' | head -c 120)"
	fi

	# Turbo: run the same frames with the core's drawing switched off for the
	# first half and back on for the second. The machine, the sound, the lag
	# count and every picture of that second half must be what they would have
	# been. A core that got this wrong shows up here as a different picture even
	# when every byte of RAM still agrees.
	if "$nat/run-wbx" "$gst/core.wbx" "$rom" "$frames" 2>/dev/null | turboDigests > "$work/norm.txt" &&
	   "$nat/run-wbx" "$gst/core.wbx" "$rom" "$frames" --turbo 2>/dev/null | turboDigests > "$work/turbo.txt"; then
		if cmp -s "$work/norm.txt" "$work/turbo.txt"; then
			report "$name:turbo" PASS "$frames frames, half of them undrawn, same machine and same pictures"
		else
			report "$name:turbo" FAIL "$(diff "$work/norm.txt" "$work/turbo.txt" | tr '\n' ' ' | head -c 120)"
		fi
	else
		report "$name:turbo" FAIL "turbo runner error"
	fi

	# The optional tooling exports the frontend probes for: absence is allowed,
	# but a core that claims a surface must render one.
	if [ -x "$nat/run-tooling" ]; then
		if "$nat/run-tooling" "$gst/core.wbx" "$rom" 120 > "$work/tool.txt" 2>&1; then
			if grep -q "RENDER FAILED" "$work/tool.txt"; then
				report "$name:tooling" FAIL "a declared surface did not render"
			else
				report "$name:tooling" PASS "$(grep -c '^  \[' "$work/tool.txt") tooling entries reported"
			fi
		else
			report "$name:tooling" FAIL "runner error"
		fi
	fi
done

echo ""
echo "$ok ok, $failed failed"
[ "$failed" -gt 0 ] && exit 1
exit 0
