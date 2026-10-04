#!/usr/bin/env bash
# Host (workstation) build of the WiNspire emulator core.
#
# Compiles the *same* core sources and the *same* release configuration as the
# calculator build so that host measurements are representative, then links the
# headless frontend in source/host/main.c on top.
#
# Usage:
#   source/build-scripts/build_host.sh [repo] [core] [frontend] [build] [extra-cflags...]
set -euo pipefail

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=${1:-$(cd -- "$script_dir/../.." && pwd)}
core=${2:-$repo/source/winspire}
frontend=${3:-$repo/source/host/main.c}
build=${4:-$repo/build}
shift 4 2>/dev/null || true
extra_flags=("$@")

work=$build/host-work
out=$build/Host
version=${WINSPIRE_VERSION:-1.0.0}
cc=${HOST_CC:-cc}

for path in "$core/i386.c" "$core/pc.c" "$frontend"; do
	[ -e "$path" ] || { echo "missing required path: $path" >&2; exit 1; }
done

rm -rf "$work" "$out"
mkdir -p "$work/obj" "$out"

cpu_flags=(-O3 -fomit-frame-pointer)
if [ -z "${HOST_NO_NATIVE:-}" ]; then
	if "$cc" -march=native -E -x c /dev/null >/dev/null 2>&1; then
		cpu_flags+=(-march=native)
	fi
fi

# Mirrors build_native.sh: WINSPIRE_NATIVE_BUILD forces release_config.h, which
# selects TINY386_SPEED_BUILD / TINY386_NO_LOG / BPP / SCALE_2_1 / BUILD_NSPIRE.
common_flags=(
	-std=c99 -g -Wall -D_POSIX_C_SOURCE=200809L
	-Wno-format -Wno-unused-function -Wno-unused-variable -Wno-unused-result
	-I"$core" -I"$repo/source/host/shim"
	-ffunction-sections -fdata-sections
	-DWINSPIRE_NATIVE_BUILD -include "$core/release_config.h"
	-DTINY386_INPUT_POLL_LOOPS=2U
	-DTINY386_VIDEO_POLL_LOOPS=1U
	"-DTINY386_VERSION=\"$version\""
	# Exposes cxlink_selftest() so the bridge protocol and the audio routing
	# can be exercised without a calculator, and bridge_net_selftest() so the
	# ESP32's DHCP server and guest-frame routing can be. See source/host/main.c
	# --selftest.
	-DCXLINK_ENABLE_SELFTEST
	-DBRIDGE_NET_ENABLE_SELFTEST
)

core_sources=(
	ini.c i386.c i8259.c i8254.c ide.c vga.c i8042.c misc.c i8257.c
	pcspk.c adlib.c ne2000.c sb16.c pc.c pci.c fpu.c fmopl.c cxlink.c
	bridge_net.c
)
objects=()

"$cc" "${common_flags[@]}" "${cpu_flags[@]}" "${extra_flags[@]}" \
	-c "$frontend" -o "$work/obj/host_main.o"
objects+=("$work/obj/host_main.o")

for source in "${core_sources[@]}"; do
	object="$work/obj/${source%.c}.o"
	extra=()
	case "$source" in
		i386.c|pc.c|vga.c|ide.c|i8254.c|i8042.c|i8259.c|misc.c)
			extra=("${cpu_flags[@]}")
			;;
		# fmopl is the OPL2 synth; it is float-heavy and not the target of
		# ARM tuning, so build it in the same bulk pass as the rest.
	esac
	"$cc" "${common_flags[@]}" "${extra[@]}" "${extra_flags[@]}" \
		-c "$core/$source" -o "$object"
	objects+=("$object")
done

# The input self test lives beside the frontend rather than in source/winspire
# because it is only meaningful on a host: it stands in for the calculator's
# keypad and touchpad, which do not exist here. It links the real i8042.c, so
# the scancodes it checks are the ones the guest actually receives.
"$cc" "${common_flags[@]}" -I"$repo/source/ndless-stub/include" -I"$repo/source/winspire-ndless" -c "$repo/source/host/input_selftest.c" \
	-o "$work/obj/input_selftest.o"
objects+=("$work/obj/input_selftest.o")

"$cc" "${objects[@]}" -o "$out/winspire-host" -Wl,--gc-sections -lm
echo "built $out/winspire-host"
