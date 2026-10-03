#!/usr/bin/env bash
# Build nspire95-cx.tns for the original TI-Nspire CX (ARM926EJ-S, Ndless).
#
# Usage:
#   source/build-scripts/build_cx.sh [profile] [repo] [build]
#
#   profile   DEBUG | RELEASE | TURBO   (default TURBO)
#
# Toolchain: the Ndless SDK (https://github.com/ndless-nspire/Ndless).
# Set NDLESS_SDK if it is not at ~/Ndless/ndless-sdk.
#
# Output in build/CX/:
#   nspire95-cx.tns     the emulator
#   nspire95-cx.elf     same build with symbols; keep it for crash forensics
#                       on the first hardware session
#   bios.bin.tns        companion BIOS image (see docs, BIOS is not bundled
#                       here for licensing reasons unless legally redistributable)
#   vgabios.bin.tns     companion VGA BIOS image
#   winspire.ini.tns    default configuration
#   bench386.img.tns    bootable smoke-test floppy (from `make bench`)
set -euo pipefail

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_default=$(cd -- "$script_dir/../.." && pwd)
profile=$(echo "${1:-TURBO}" | tr '[:lower:]' '[:upper:]')
repo=${2:-$repo_default}
build=${3:-$repo/build}
core=$repo/source/winspire
frontend=$repo/source/winspire-ndless/main.c
sdk=${NDLESS_SDK:-$HOME/Ndless/ndless-sdk}
work=$build/cx-work
out=$build/CX
version=${WINSPIRE_VERSION:-1.0.0}

case "$profile" in
	DEBUG|RELEASE|TURBO) ;;
	*) echo "unknown profile '$profile' (expected DEBUG, RELEASE or TURBO)" >&2; exit 1 ;;
esac

export PATH="$sdk/bin:$PATH"
for path in "$core/i386.c" "$core/pc.c" "$frontend" "$core/cx_profiles.h" \
	"$sdk/bin/nspire-gcc" "$sdk/bin/nspire-ld" "$sdk/bin/genzehn" \
	"$sdk/bin/make-prg"; do
	[ -e "$path" ] || { echo "missing required path: $path" >&2; exit 1; }
done

rm -rf "$work" "$out"
mkdir -p "$work/obj" "$out"

cc=$sdk/bin/nspire-gcc
ld=$sdk/bin/nspire-ld

# -mcpu=arm926ej-s is correct for BOTH the original CX and the CX II: the
# CX II is also an ARM926EJ-S, just clocked ~3x higher. It also keeps generated
# code free of ARMv6/ARMv7-only instructions, which is required for the CX.
common_flags=(
	-std=c99 -marm -mcpu=arm926ej-s -mtune=arm926ej-s
	-Os -g -Wall
	-Wno-format -Wno-unused-function -Wno-unused-variable
	-I"$core"
	"-ffile-prefix-map=$repo/="
	"-fdebug-prefix-map=$repo/="
	"-fmacro-prefix-map=$repo/="
	-ffunction-sections -fdata-sections
	-DWINSPIRE_NATIVE_BUILD
	-include "$core/release_config.h"
	-include "$core/cx_profiles.h"
	-D"WINSPIRE_PROFILE_$profile"
	"-DTINY386_VERSION=\"$version\""
	# cxlink bridges the guest NE2000 and the Sound Blaster mixer to an ESP32
	# on the dock connector. Add -DWINSPIRE_CXLINK_UART once the UART base has
	# been confirmed on hardware; see HARDWARE.md.
	-DUSE_CXLINK
)

case "$profile" in
	DEBUG)
		hot_flags=(-O1 -fno-omit-frame-pointer)
		;;
	*)
		# Hot paths get -O3; the rest of the core stays at -Os so the image
		# still fits comfortably in the CX's flash-resident .tns.
		hot_flags=(-O3 -fomit-frame-pointer)
		;;
esac

# Hot (interpreter + timing-critical devices) and cold (initialisation,
# diagnostics, INI parsing, GUI) sources are separated so that
# -ffunction-sections + --gc-sections can drop unused diagnostics and so the
# hot code lands together for better ARM926 I-cache locality.
hot_sources=(i386.c pc.c vga.c ide.c i8254.c i8042.c i8259.c misc.c)
cold_sources=(ini.c i8257.c pcspk.c adlib.c pci.c cxlink.c)
# Sound Blaster and the NIC sit on the audio/network hot paths in RELEASE and
# TURBO; in DEBUG they stay cold so the traces are easier to follow.
if [ "$profile" = DEBUG ]; then
	cold_sources+=(sb16.c ne2000.c)
else
	hot_sources+=(sb16.c ne2000.c)
fi

core_sources=("${hot_sources[@]}" "${cold_sources[@]}")
hot_set=" ${hot_sources[*]} "
objects=()

"$cc" "${common_flags[@]}" "${hot_flags[@]}" -c "$frontend" \
	-o "$work/obj/nspire_main.o"
objects+=("$work/obj/nspire_main.o")

for source in "${core_sources[@]}"; do
	object="$work/obj/${source%.c}.o"
	extra=()
	case "$hot_set" in
		*" $source "*) extra=("${hot_flags[@]}") ;;
	esac
	"$cc" "${common_flags[@]}" "${extra[@]}" -c "$core/$source" -o "$object"
	objects+=("$object")
done

"$ld" "${objects[@]}" -o "$work/nspire95-cx.elf" -Wl,--gc-sections -lm

"$sdk/bin/genzehn" --input "$work/nspire95-cx.elf" \
	--output "$work/nspire95-cx.tns.zehn" \
	--name "nspire95-cx $version ($profile)" \
	--author "Malik Idrees Hasan Khan" \
	--notice "x86 PC emulator for the TI-Nspire CX (ARM926EJ-S)" \
	--ndless-min 42 --ndless-rev-min 2004 --uses-lcd-blit 1
"$sdk/bin/make-prg" "$work/nspire95-cx.tns.zehn" "$out/nspire95-cx.tns"
rm -f "$work/nspire95-cx.tns.zehn"

# Companion files. The BIOS images are NOT generated here: ship them only if
# their license permits redistribution (see README_CX.md).
install -m 0644 "$core/bios.bin" "$out/bios.bin.tns"
install -m 0644 "$core/vgabios.bin" "$out/vgabios.bin.tns"
# native.ini.tns *is* the shipping configuration: it boots the test floppy out
# of the box and documents the one-line switch to a Windows 95 hard disk.
install -m 0644 "$core/native.ini.tns" "$out/winspire.ini.tns"
if [ -f "$build/bench/bench386.img" ]; then
	install -m 0644 "$build/bench/bench386.img" "$out/bench386.img.tns"
else
	echo "note: $build/bench/bench386.img missing; run 'make bench' for the test floppy" >&2
fi
# Unstripped ELF: -g symbols are the only way to make sense of a crash on
# hardware, where there is no debugger and no core dump.
install -m 0644 "$work/nspire95-cx.elf" "$out/nspire95-cx.elf"

size=$(wc -c < "$out/nspire95-cx.tns")
rm -rf "$work"
echo "built $out/nspire95-cx.tns ($size bytes, $profile profile)"
