#!/bin/sh
# Builds core.wbx - quickerNES as a miniHawk waterbox core - plus the two drivers
# used by the equivalence gate (run-wbx against the sandbox, run-native against
# the original shared library).
#
# Prereq: a miniBox checkout built WITH the C++ guest toolchain, since quickerNES
# is C++ (Tier 2: STL, no exceptions):
#   meson setup <miniBox>/build/meson-cpp -Dguest_cpp=true
#   ninja -C <miniBox>/build/meson-cpp
#
# Usage: ./build-core.sh [-m <miniBox dir>] [-o <output dir>]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-$HOME/miniBox}"
out="$here/bin"
while getopts "m:o:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		o) out="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
mb="$(cd "$mb" && pwd)"
mbuild="$mb/build/meson-cpp"
sr="$mbuild/guest-sysroot"
gccver="$(gcc -dumpfullversion)"

[ -f "$sr/lib/libstdc++.a" ] || {
	echo "miniBox C++ guest toolchain missing at $sr." >&2
	echo "Run: meson setup $mbuild $mb -Dguest_cpp=true && ninja -C $mbuild" >&2
	exit 1
}

mkdir -p "$out/obj"

# _QUICKERNES_ENABLE_TRACEBACK_SUPPORT turns on the core's per-instruction trace
# hook, without which set_tracecb is bound but never fires (it is defined nowhere
# else in the repo, so the native build's tracing is inert). It costs one
# predictable branch per instruction and does not change emulation output - the
# equivalence gate is re-run with it on.
#
# The guest flags: miniBox's frozen waterbox flags plus quickerNES's own
# -fno-strict-aliasing (the core type-puns the cart header; without this the
# iNES header is misparsed under -O2). __JAFFAR_COMMON_INLINE__ is normally
# defined by extern/jaffarCommon/meson.build.
cflags="-fvisibility=hidden -mcmodel=large -mstack-protector-guard=global \
	-fno-pic -fno-pie -fcf-protection=none -O2 -DNDEBUG -std=c++20 \
	-fno-exceptions -fno-rtti -fno-strict-aliasing \
	-D_QUICKERNES_ENABLE_TRACEBACK_SUPPORT"
incs="-I$sr/include/c++/$gccver -I$sr/include/c++/$gccver/x86_64-linux-musl \
	-I$mb/extern/emulibc -I$mb/source/guest/include -I$mb/extern/jsmn \
	-I$root/source -I$root/source/quickerNES -I$root/source/quickerNES/core \
	-I$root/extern/jaffarCommon/include"
jaffar='-D__JAFFAR_COMMON_INLINE__=__attribute__((__used__)) inline'
specs="-specs $sr/lib/musl-gcc.specs"

# guest objects: the unmodified core + the waterbox ABI layer
srcs="$(find "$root/source/quickerNES" -name '*.cpp' | sort) $here/quickernes_wbx.cpp"
for f in $srcs; do
	o="$out/obj/$(echo "${f#$root/}" | tr '/' '_').o"
	g++ $specs $cflags $incs "$jaffar" -c -o "$o" "$f"
done

# The link recipe (library order, --no-relax, the weak pthread pulls) comes from
# miniBox's guest kit; see source/guest/meson.build there for why each is needed.
ldflags="-static -no-pie -Wl,--eh-frame-hdr,-O2,--no-relax -T $mb/source/guest/linkscript.T \
	-Wl,-u,pthread_once -Wl,-u,pthread_cond_wait -Wl,-u,pthread_cond_broadcast -Wl,-u,pthread_key_create"
g++ $specs -mcmodel=large -fno-pic -fno-pie $ldflags -o "$out/core.wbx" \
	"$out"/obj/*.o "$mbuild/source/guest/cxxglue.c.o" "$mbuild/source/guest/emulibc.c.o" \
	-L"$sr/lib" -lstdc++ -lgcc -lgcc_eh -lc
echo "built $out/core.wbx"

# drivers for the equivalence gate
gcc -O2 -Wall -I"$mb/source/host" -o "$out/run-wbx" "$here/run-wbx.c" \
	"$mbuild/source/host/libminiboxhost.so" -Wl,-rpath,"$mbuild/source/host"
gcc -O2 -Wall -o "$out/run-native" "$here/run-native.c" -ldl
echo "built $out/run-wbx and $out/run-native"

# the package the frontend loads: core.wbx (fixed name) + waterbox.config
cp "$here/waterbox.config" "$out/waterbox.config"
echo "package files ready: $out/core.wbx + $out/waterbox.config"
