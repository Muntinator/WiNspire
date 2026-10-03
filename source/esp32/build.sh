#!/usr/bin/env bash
#
# Build the cxlink ESP32 bridge firmware.
#
# This one lives with the firmware rather than in source/build-scripts/ because
# ESP-IDF is not a normal cross-compiler: it must be run from the project
# directory (it reads CMakeLists.txt and writes sdkconfig here), so the script
# has to know its own project root anyway. The calculator-side scripts
# (build_cx.sh, check_cx_frontend.sh) stay in source/build-scripts/.
#
# Usage:
#   source/esp32/build.sh [target] [build-dir] [--clean]
#
#   target     ESP32 variant, default "esp32". Only classic ESP32 is described
#              by HARDWARE.md; other targets need their own pin plan.
#   build-dir  default <repo>/build/esp32 (matches build/Host, build/CX)
#   --clean    fullclean first, then build from scratch
#
# Environment:
#   IDF_PATH        ESP-IDF checkout        (default /tmp/esp-idf)
#   IDF_TOOLS_PATH  idf_tools.py toolchain  (default /tmp/espressif)
#
# One-time prerequisites, in order:
#
#   1. Clone ESP-IDF. v5.x is required: main.c uses the v5 driver/i2s_std.h API,
#      not the legacy driver/i2s.h one.
#
#        git clone --depth 1 --branch v5.5.5 \
#            --recurse-submodules --shallow-submodules \
#            https://github.com/espressif/esp-idf.git /tmp/esp-idf
#
#   2. Install the toolchain (xtensa-esp-elf, esp-rom-elfs, python env):
#
#        IDF_PATH=/tmp/esp-idf IDF_TOOLS_PATH=/tmp/espressif \
#            /tmp/esp-idf/install.sh esp32
#
#   3. Make cmake, ninja and libusb available. On Linux these come from the
#      system, NOT from install.sh, and a minimal container has none of them.
#      Both of these failure modes were hit bringing this firmware up:
#
#        apt-get update && apt-get install -y cmake ninja-build libusb-1.0-0
#
#      - cmake / ninja: install.sh does not provide them on Linux, so `idf.py`
#        fails at configure time with "cmake not found".
#      - libusb-1.0.so.0: without it, install.sh's version check on
#        openocd-esp32 fails (openocd is JTAG-only and irrelevant to building),
#        install.sh exits non-zero, and — the trap — it then never reaches the
#        "Installing Python environment" step. export.sh afterwards reports
#        "tool openocd-esp32 has no installed versions" and refuses to
#        activate. If you cannot install libusb, run these two directly:
#
#          IDF_PATH=/tmp/esp-idf IDF_TOOLS_PATH=/tmp/espressif \
#              python3 /tmp/esp-idf/tools/idf_tools.py install esp-rom-elfs
#          IDF_PATH=/tmp/esp-idf IDF_TOOLS_PATH=/tmp/espressif \
#              python3 /tmp/esp-idf/tools/idf_tools.py install-python-env
#
# Flashing (the pins and level shifting are in HARDWARE.md section 3):
#
#   IDF_PATH=... . /tmp/esp-idf/export.sh
#   idf.py -B build/esp32 -p /dev/ttyUSB0 flash monitor
#
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../.." && pwd)

target=esp32
build_dir=$repo/build/esp32
clean=0

args=()
for arg in "$@"; do
	case "$arg" in
		--clean) clean=1 ;;
		-h|--help)
			sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
			exit 0
			;;
		*) args+=("$arg") ;;
	esac
done
if [ "${#args[@]}" -gt 0 ]; then
	target=${args[0]}
fi
if [ "${#args[@]}" -gt 1 ]; then
	build_dir=${args[1]}
fi

idf_path=${IDF_PATH:-/tmp/esp-idf}
tools_path=${IDF_TOOLS_PATH:-/tmp/espressif}

export IDF_PATH=$idf_path
export IDF_TOOLS_PATH=$tools_path

# The shared wire protocol is compiled into this firmware (see main/CMakeLists.txt).
for path in "$script_dir/CMakeLists.txt" "$script_dir/main/main.c" \
	    "$script_dir/main/CMakeLists.txt" "$repo/source/winspire/cxlink.c" \
	    "$repo/source/winspire/cxlink.h"; do
	[ -e "$path" ] || { echo "missing required file: $path" >&2; exit 1; }
done

if [ ! -f "$idf_path/export.sh" ]; then
	cat >&2 <<-EOF
	ESP-IDF not found at IDF_PATH=$idf_path

	Install it once, then re-run this script:

	  git clone --depth 1 --branch v5.5.5 \\
	      --recurse-submodules --shallow-submodules \\
	      https://github.com/espressif/esp-idf.git $idf_path
	  IDF_PATH=$idf_path IDF_TOOLS_PATH=$tools_path $idf_path/install.sh esp32

	The v5.x branch is required (driver/i2s_std.h). See the header of this
	script for the library prerequisites that are easy to miss.
	EOF
	exit 1
fi

if ! command -v cmake >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
	echo "cmake and ninja are required and are not on PATH." >&2
	echo "On Debian/Ubuntu: apt-get install -y cmake ninja-build" >&2
	exit 1
fi

# shellcheck disable=SC1090
. "$idf_path/export.sh" >/dev/null

cd "$script_dir"

# idf.py set-target rewrites sdkconfig and forces a full rebuild, so only do it
# when the recorded target is missing or different. This is what keeps repeat
# builds incremental.
if [ "$clean" -eq 1 ]; then
	idf.py -B "$build_dir" fullclean
fi
if [ "$clean" -eq 1 ] || [ ! -f "$script_dir/sdkconfig" ] || \
   ! grep -q "^CONFIG_IDF_TARGET=\"$target\"\$" "$script_dir/sdkconfig"; then
	echo "configuring for target $target"
	idf.py -B "$build_dir" set-target "$target" >/dev/null
fi

idf.py -B "$build_dir" build

echo
echo "flashable images in $build_dir:"
for image in bootloader/bootloader.bin partition_table/partition-table.bin \
	     cxlink-bridge.bin; do
	if [ -f "$build_dir/$image" ]; then
		size=$(stat -c %s "$build_dir/$image" 2>/dev/null || wc -c <"$build_dir/$image")
		printf '  %-42s %8s bytes\n' "$image" "$size"
	fi
done
echo
echo "flash with:"
echo "  idf.py -B $build_dir -p /dev/ttyUSB0 flash monitor"
