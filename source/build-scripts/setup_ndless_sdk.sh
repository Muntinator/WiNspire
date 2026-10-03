#!/usr/bin/env bash
# Assemble a working Ndless SDK for building nspire95-cx.tns, without waiting
# for the upstream toolchain build (which compiles its own binutils+gcc+newlib
# from source and takes the better part of an hour).
#
# What this does:
#   1. Installs Ubuntu's arm-none-eabi GCC (10.3) + newlib, plus the host
#      libraries genzehn needs (Boost.Program_options, zlib).
#   2. Clones the Ndless SDK sources and the two submodules the build needs
#      (nspire-io for headers/lib, zlib for the compressed zehn loader).
#   3. Builds libsyscalls, libndls, libnspireio, genzehn, the system crt
#      objects and the zehn loaders - everything build_cx.sh links against.
#   4. Applies four compatibility fixes the modern environment needs; each is
#      marked FIX below and explained where it happens.
#
# Usage:
#   source/build-scripts/setup_ndless_sdk.sh [install-dir]   (default ~/Ndless)
#
# Then either export NDLESS_SDK=<install-dir>/Ndless/ndless-sdk, or let
# build_cx.sh find it at ~/Ndless/ndless-sdk, and run `make cx`.
set -euo pipefail

install_root=${1:-$HOME/Ndless}
tree=$install_root/Ndless
sdk=$tree/ndless-sdk
ndless_url=${NDLESS_URL:-https://github.com/ndless-nspire/Ndless.git}

# ---------------------------------------------------------------- packages --
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
	echo "== installing arm-none-eabi toolchain (needs root) =="
	DEBIAN_FRONTEND=noninteractive apt-get install -y \
		gcc-arm-none-eabi libnewlib-arm-none-eabi
fi
# genzehn is a host tool: ELF parsing (elfio, vendored) + Boost + zlib.
if ! echo 'int main(){}' | g++ -x c++ - -lboost_program_options -lz \
		-o /dev/null 2>/dev/null; then
	echo "== installing genzehn host dependencies (needs root) =="
	DEBIAN_FRONTEND=noninteractive apt-get install -y \
		libboost-program-options-dev zlib1g-dev
fi

# ----------------------------------------------------------------- sources --
if [ ! -d "$sdk" ]; then
	echo "== cloning Ndless SDK =="
	mkdir -p "$install_root"
	git clone --depth 1 "$ndless_url" "$tree"
fi
git -C "$tree" submodule update --init --depth 1 \
	ndless-sdk/thirdparty/nspire-io
git -C "$tree" submodule update --init --depth 1 \
	ndless-sdk/thirdparty/zlib

export PATH="$sdk/bin:$PATH"

# ------------------------------------------------------------------- FIX 1 --
# libsyscalls has a rule that regenerates a header through php when stubs.cpp
# is older than its inputs. A fresh checkout timestamps them equally and the
# generated header is already committed, so touching stubs.cpp keeps the build
# hermetic (no php needed).
touch "$sdk/libsyscalls/stubs.cpp"

# ------------------------------------------------------------------- FIX 2 --
# libsyscalls' stdlib.cpp uses PATH_MAX, which newlib 3.3 no longer provides
# through limits.h. Defining it on the command line is what older newlib did
# (POSIX guarantees 4096) and touches no upstream file.
path_max_flags="-DPATH_MAX=4096"
make -C "$sdk/libsyscalls" \
	CFLAGS="-mcpu=arm926ej-s -std=c11 -nostdlib -O3 -fPIE -mlong-calls -Wall -Werror -I ../include/ -I ../thirdparty/nspire-io/include/ -D_TINSPIRE $path_max_flags -ffunction-sections -fdata-sections" \
	CXXFLAGS="-mcpu=arm926ej-s -std=c++11 -nostdlib -O3 -fPIE -fno-exceptions -fno-rtti -mlong-calls -Wall -Werror -I ../include/ -I ../thirdparty/nspire-io/include/ -D_TINSPIRE $path_max_flags -ffunction-sections -fdata-sections"

# nspire-io: only the library target; the demo programs need libndls, which is
# not built yet at this point.
make -C "$sdk/thirdparty/nspire-io" lib
cp "$sdk/thirdparty/nspire-io/lib/libnspireio.a" "$sdk/lib/"

make -C "$sdk/libndls"
make -C "$sdk/tools/genzehn"

# ------------------------------------------------------------------- FIX 3 --
# system/ldscript sorts .fini_array with SORT_BY_INIT_PRIORITY(REVERSE(...)),
# which needs binutils >= 2.39; Ubuntu ships 2.38. The only .fini_array
# content is this SDK's own hand-rolled dtor list, so a plain wildcard is
# equivalent here. Harmless on newer binutils too.
sed -i 's/KEEP(\*(SORT_BY_INIT_PRIORITY(REVERSE(\.fini_array\.\*))))/KEEP(*(.fini_array.*))/' \
	"$sdk/system/ldscript"
make -C "$sdk/system"

# ------------------------------------------------------------------- FIX 4 --
# Ubuntu's newlib 3.3 pulls libc's fini.o into every link, which wants _init
# and _fini; the Ndless crt does its own ctor/dtor handling and defines
# neither. Empty weak stubs satisfy the reference without changing behaviour.
if ! grep -q 'weak _fini' "$sdk/system/crtn.S"; then
	cat >> "$sdk/system/crtn.S" <<'EOF'

/* newlib 3.3 pulls in libc's fini.o, which wants these. The Ndless crt does
 * its own ctor/dtor handling, so empty stubs are all that is needed. */
.section .text
_init: .weak _init
	bx lr
_fini: .weak _fini
	bx lr
EOF
	make -C "$sdk/system" clean all
fi

# zehn loaders. The compressed variant compiles zlib in-tree and expects the
# headers next to the objects (zlib's configure does not copy them); the build
# directory itself is created by the zlib configure rule, so create it here.
mkdir -p "$sdk/tools/zehn_loader/zlib"
cp "$sdk/thirdparty/zlib/zlib.h" "$sdk/tools/zehn_loader/zlib/"
[ -f "$sdk/tools/zehn_loader/zlib/zconf.h" ] || \
	cp "$sdk/thirdparty/zlib/zconf.h" "$sdk/tools/zehn_loader/zlib/"
make -C "$sdk/tools/zehn_loader"

echo
echo "Ndless SDK ready: $sdk"
echo "Build with:  NDLESS_SDK=$sdk bash source/build-scripts/build_cx.sh TURBO"
echo "or:          NDLESS_SDK=$sdk make cx"
