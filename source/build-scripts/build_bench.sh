#!/usr/bin/env bash
# Assemble the x86 benchmark payload and wrap it in a bootable floppy image.
#
# Produces build/bench/bench386.img: a 1.44 MiB FAT-less floppy image whose
# first sector is the benchmark boot program. The emulated BIOS boots it.
set -euo pipefail

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$script_dir/../.." && pwd)
src=$repo/source/bench/bench386.S
out_dir=$repo/build/bench
obj=$out_dir/bench386.o
bin=$out_dir/bench386.bin
img=$out_dir/bench386.img

as_bin=${AS:-as}
ld_bin=${LD:-ld}
cc_bin=${HOST_CC:-cc}

mkdir -p "$out_dir"

# Prefer the host toolchain's 32-bit assembler; gcc is used only to locate it.
if ! command -v "$as_bin" >/dev/null; then
	echo "missing assembler: $as_bin" >&2
	exit 1
fi

"$as_bin" --32 -o "$obj" "$src"
"$ld_bin" -m elf_i386 -Ttext 0x7c00 --oformat binary -o "$bin" "$obj"

size=$(wc -c < "$bin")
if [ "$size" -ne 512 ]; then
	echo "boot sector must be exactly 512 bytes, got $size" >&2
	exit 1
fi

rm -f "$img"
# 1.44 MiB 3.5" floppy geometry (2880 x 512), first sector = boot program.
dd if=/dev/zero of="$img" bs=512 count=2880 status=none
dd if="$bin" of="$img" bs=512 count=1 conv=notrunc status=none

echo "built $img ($(wc -c < "$img") bytes)"
