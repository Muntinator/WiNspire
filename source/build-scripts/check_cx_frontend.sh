#!/usr/bin/env bash
# Type-check the CX frontend and the shared core without the Ndless SDK.
#
# The SDK is only needed to *produce* the .tns; parsing the sources does not
# need ARM or Ndless, because nothing in the frontend depends on ARM-specific
# code. This script compiles every translation unit the CX build uses with the
# host compiler, using the typecheck-only stub in source/ndless-stub/include.
#
# It catches signature drift against the Ndless API (which is the main risk in
# the CX port) and any C errors introduced in the shared core.
#
# Usage: source/build-scripts/check_cx_frontend.sh [repo] [profile]
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=${1:-$(cd -- "$script_dir/../.." && pwd)}
profile=$(echo "${2:-TURBO}" | tr '[:lower:]' '[:upper:]')
core=$repo/source/winspire
stub=$repo/source/ndless-stub/include
frontend=$repo/source/winspire-ndless/main.c
cc=${HOST_CC:-cc}

case "$profile" in
	DEBUG|RELEASE|TURBO) ;;
	*) echo "unknown profile '$profile'" >&2; exit 1 ;;
esac

flags=(
	-fsyntax-only -std=c99 -Wall -Wno-unused-function -Wno-unused-variable
	-Wno-unused-const-variable -Wno-format
	-I"$stub" -I"$core"
	-DWINSPIRE_NATIVE_BUILD
	-include "$core/release_config.h"
	-include "$core/cx_profiles.h"
	-D"WINSPIRE_PROFILE_$profile"
	-DUSE_CXLINK
	-DTINY386_VERSION='"0.0.0-check"'
)

sources=(
	"$frontend"
	"$core/ini.c" "$core/i386.c" "$core/i8259.c" "$core/i8254.c"
	"$core/ide.c" "$core/vga.c" "$core/i8042.c" "$core/misc.c"
	"$core/i8257.c" "$core/pcspk.c" "$core/adlib.c" "$core/ne2000.c"
	"$core/sb16.c" "$core/pc.c" "$core/pci.c" "$core/cxlink.c"
	# bridge_net.c is not compiled into the .tns; it runs on the ESP32. It is
	# shared source compiled by the firmware and the host build, and it is
	# freestanding, so checking it here keeps every shared translation unit
	# ARM-clean with one command.
	"$core/bridge_net.c"
)

status=0
for source in "${sources[@]}"; do
	# fpumopl.c is only pulled into the server build; skip it here.
	[ -e "$source" ] || { echo "missing $source" >&2; status=1; continue; }
	if ! output=$("$cc" "${flags[@]}" "$source" 2>&1); then
		echo "FAIL $(basename "$source")"
		echo "$output" | head -25
		status=1
	else
		echo "ok   $(basename "$source")"
	fi
done

if [ "$status" -eq 0 ]; then
	echo "CX frontend + core: type-check clean ($profile profile)"
fi
exit "$status"
