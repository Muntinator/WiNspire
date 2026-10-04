# README_CX.md — nspire95-cx: a PC emulator for the original TI-Nspire CX

`nspire95-cx` runs a complete emulated x86 PC on a **TI-Nspire CX** (the original
CX, not the CX II), with the goal of making Windows 95 genuinely usable on it.
It is a port of WiNspire / tiny386 to the original CX's hardware.

The emulator is complete, not a compatibility layer: the guest gets a real CPU
(real mode, protected mode, paging), BIOS, RAM, VGA, IDE, keyboard, mouse, PIC,
PIT, DMA, CMOS/RTC, a Sound Blaster 16 and an NE2000 Ethernet adapter. Windows 95
runs its own drivers and its own TCP/IP stack.

> **Read [CX_PORT_STATUS.md](CX_PORT_STATUS.md) before building.** This port has
> been type-checked and benchmarked, but it has **not** been run on a calculator.
> That document says exactly what is verified and what is not.

---

## 1. Requirements

| To build | To run |
|---|---|
| The [Ndless SDK](https://github.com/ndless-nspire/Ndless) (`nspire-gcc`, `nspire-ld`, `genzehn`, `make-prg`) plus an ARM cross compiler — `source/build-scripts/setup_ndless_sdk.sh` assembles the whole toolchain in one step | A TI-Nspire **CX** or **CX CAS** |
| Bash | Ndless 4.x installed |
| ~200 MB disk | A legally obtained Windows 95 disk image |
| | (optional) an ESP32 for audio and networking (firmware in `source/esp32/`) |

A grayscale Clickpad/Touchpad cannot run this: its PL110 panel is 4bpp gray and
cannot display the 16bpp guest surface.

## 2. Build

No toolchain handy? Prebuilt calculator files are attached to every GitHub
release (`nspire95-cx-calculator.zip`) — you can skip this whole section.

To build yourself, assemble the toolchain once (Debian/Ubuntu; needs root for
the package step, about a minute):

```sh
source/build-scripts/setup_ndless_sdk.sh      # installs to ~/Ndless
```

Then:

```sh
export NDLESS_SDK=$HOME/Ndless/ndless-sdk     # optional if that is the location

# TURBO is the default and is what you want on a CX.
source/build-scripts/build_cx.sh TURBO

# Other profiles:
source/build-scripts/build_cx.sh RELEASE
source/build-scripts/build_cx.sh DEBUG
```

Output lands in `build/CX/`:

```
nspire95-cx.tns        the emulator (TURBO profile)
nspire95-cx.elf        the same build with symbols, for crash forensics
bios.bin.tns           companion BIOS image
vgabios.bin.tns        companion VGA BIOS image
winspire.ini.tns       default configuration (boots the test floppy)
bench386.img.tns       bootable smoke-test floppy (from `make bench`)
```

### Profiles

All three profiles emulate the **same complete hardware**. The profiles change
only diagnostic and presentation work plus instruction-batch sizes — TURBO is not
a stripped-down mode, and the brief's non-negotiable principle is enforced by
construction in `source/winspire/cx_profiles.h`.

| Profile | Optimisation | Batching | Use |
|---|---|---|---|
| `DEBUG` | `-O1`, no frame-pointer omission, instruction tracing, 1-instruction batches | trace-accurate | Bringing up the port, diagnosing a hang; the trace goes to `winspire.log.tns` |
| `RELEASE` | `-Os` core, `-O3` hot paths | moderate | General use |
| `TURBO` | `-Os` core, `-O3` hot paths, interpreter fast paths, fastest input sampling | large | Default; fastest desktop |

Every profile compiles for `-marm -mcpu=arm926ej-s -mtune=arm926ej-s`, which is
correct for the CX (and for the CX II, which shares the core) and guarantees no
ARMv6/ARMv7-only instruction can appear.

### Type-check without the SDK

The Ndless API is stubbed in `source/ndless-stub/`, so the frontend and the
entire emulator core can be checked without a toolchain:

```sh
source/build-scripts/check_cx_frontend.sh             # TURBO
source/build-scripts/check_cx_frontend.sh . DEBUG
source/build-scripts/check_cx_frontend.sh . RELEASE
```

This is how the port was validated in an environment with no Ndless SDK, and it
is a reasonable thing to wire into CI.

### Benchmark and bridge checks without a calculator

The host frontend builds the same core with the same configuration as the
calculator target, so it is the closest thing to a CX test here:

```sh
source/build-scripts/build_host.sh
source/build-scripts/build_bench.sh

# Emulation throughput, plus the cost of the audio path and the ESP32 bridge.
build/Host/winspire-host --boot build/bench/bench386.img --seconds 60

# The only executable check of the cxlink protocol: codec, ACK/retry,
# audio framing, corruption recovery, reset. No calculator, no ESP32.
build/Host/winspire-host --selftest
```

Or through make: `make host`, `make bench-run`, `make selftest`, `make check`.

See [PERFORMANCE.md](PERFORMANCE.md) for what the numbers mean and, importantly,
what they do not — in particular that host x86-64 throughput does not predict
ARM926 throughput, and that no run here has ever produced real audio content.

---

## 3. Installing on the calculator

Copy all of these to the same folder on the calculator (`nspire95-cx-calculator.zip`
from the release page is exactly this set):

```
nspire95-cx.tns        the emulator
bios.bin.tns
vgabios.bin.tns
winspire.ini.tns
bench386.img.tns       bootable test floppy
```

Then run `nspire95-cx.tns`. Out of the box it boots `bench386.img.tns`: the
emulated PC POSTs and runs a small x86 benchmark payload on the emulated
screen, which is the fastest way to see that the whole stack works before any
Windows 95 image is involved. Errors are reported through a system message box
before the LCD is taken over, so a bad path or a missing file does not leave
you with a black screen.

To boot Windows 95 instead: copy `disk.img.tns` over (see §4), comment out the
`fda` line in `winspire.ini.tns` and uncomment `hda = disk.img.tns`.

## 4. Disk image

The disk is a raw image, exactly as QEMU or VirtualBox would produce. Create and
install it on a desktop machine:

```sh
qemu-img create -f raw win95.img 512M
qemu-system-i386 -m 64 -hda win95.img -cdrom win95.iso -boot d
# install Windows 95 normally, shut down, then copy win95.img to the
# calculator as disk.img.tns
```

Notes:

- **Do not** pre-install a disk with a different emulated chipset, or the IDE
  driver will not match. Install against the same emulator or a plain 440FX
  machine.
- Keep the image at or under 512 MiB. The CX's storage and the fat filesystem are
  the practical limits, and larger images make every IDE operation slower.
- The image is opened read/write, so **back it up**. A copy is taken on every
  run, and a corrupted image is not recoverable from the calculator.

**Licensing:** this repository does not and cannot ship Windows 95 media, nor
BIOS images whose redistribution is not permitted. Supply your own.

### `winspire.ini.tns`

```ini
[pc]
bios = bios.bin.tns
vga_bios = vgabios.bin.tns
mem_size = 16M          ; 4M..32M; the CX has 64 MiB of SDRAM
vga_mem_size = 256K     ; 64K..1M
;hda = disk.img.tns     ; your Windows 95 image (see §4)
fda = bench386.img.tns  ; included test floppy; comment out when using hda
fill_cmos = 1           ; 1 for Windows 95

[display]
width = 320
height = 240

[cpu]
gen = 4                 ; 4 = 486 class, which is what Windows 95 expects
fpu = 0                 ; 0 = software FPU; 1 = emulated FPU (needed for XP)
clock_hz = 4770000      ; guest clock; 1M..100M

; Optional. Uplink credentials for the ESP32 bridge; see §6. Omit the whole
; section to run with no Wi-Fi. Never quoted, and never echoed anywhere.
[network]
ssid = MyHomeNetwork    ; 1..31 characters
password = ...          ; 8..63, or omit for an open network
```

`mem_size` is the single most useful knob. 16 MiB is the default because the
Ndless heap is what limits the emulator, not the 64 MiB of installed SDRAM; raise
it only if you see Windows 95 thrashing and the calculator still has heap.

---

## 5. Controls

### Keyboard

Mapped to PC set-1 scancodes through the emulated 8042/PS/2 controller.

| Nspire | PC |
|---|---|
| `Esc` | Esc |
| letters, digits, `,` `.` `-` `=` `/` | same |
| `Var` | `;` |
| `Space` | Space |
| `Enter` | Enter |
| `Tab` | Tab |
| `Shift` | Shift |
| `Ctrl` | Ctrl |
| `Del` | Backspace |
| `Home` | Home |
| arrows | arrows |

`Alt` and the PC function keys are not reachable from the Nspire keypad — that
is a physical limitation, not a gap in the mapping. Games and applications that
require `Alt` (for example `Alt+Tab` or `Alt+F4`) cannot be driven from the
calculator alone.

### Touchpad and mouse

The CX's capacitive touchpad becomes a PS/2 mouse:

- Fingertip drag moves the pointer, with sub-pixel remainders accumulated so slow
  drags stay smooth and an acceleration curve so a full 320-pixel sweep does not
  need a long swipe.
- Touchpad **arrow zones** are reported as keyboard arrow keys, not mouse motion,
  which is what you want for menu navigation.
- `Ctrl` + tap is a **right click**; a plain tap is a left click.
- `Ctrl` held while touching moves the mouse without clicking.

### Exiting

Press any calculator key to leave the emulator. The LCD is returned to TI-OS, the
hardware cursor (on models that have one) is restored, and the CPU clock is
restored if the port raised it.

---

## 6. Audio and networking (optional ESP32)

Both are provided by an ESP32 attached to the **dock connector** (J01: Tx pin 4,
Rx pin 3, GND pin 5, TTL 115200 8N1). The ESP32 is a peripheral, not a
co-emulator: the calculator still runs the whole PC.

- Audio: Sound Blaster PCM → cxlink → ESP32 → I2S → speaker. See
  [AUDIO_ARCHITECTURE.md](AUDIO_ARCHITECTURE.md).
- Networking: the emulated NE2000 is unchanged; its frames go over cxlink to the
  ESP32, which acts as the router Windows 95 is plugged into. The guest subnet is
  192.168.77.0/24 with the ESP32 at 192.168.77.1, a DHCP pool of
  192.168.77.100–107, and lwIP forwarding plus source NAT onto the ESP32's Wi-Fi
  station link. See [NETWORK_ARCHITECTURE.md](NETWORK_ARCHITECTURE.md).
- Wiring and pinouts: [HARDWARE.md](HARDWARE.md).
- Firmware: `source/esp32/` — build it with `bash source/esp32/build.sh`
  (or `make esp32`); setup and wiring in `source/esp32/README.md`. It compiles
  clean but has never been flashed to a board.

### Provisioning the Wi-Fi uplink

The bridge has no console, so it takes the access point's SSID and password from
the calculator over the same link. Add a `[network]` section to `winspire.ini`
(`winspire.ini.tns` is generated from `source/winspire/native.ini.tns`, which
shows the section commented out):

```ini
[network]
ssid = MyHomeNetwork
password = correcthorsebatterystaple
```

`ssid` is 1-31 characters; `password` is 8-63 characters, or leave it out for an
open network. Leave `ssid` out to run with no Wi-Fi. Values are taken literally,
so do not quote them and keep a space before any trailing comment.

The credentials are validated before the emulator starts (a bad entry is a
message box, not a silent failure), sent to the bridge as soon as the link comes
up, and re-sent until the bridge confirms it holds them — which also means an
ESP32 that is unplugged and restarted gets them again automatically. They are
never written to flash on the bridge, never logged, and never printed; the
calculator wipes its own copy when it exits. Nothing else reads the file.

If the ESP32 refuses them (a wrong password, say), the exit message says so
rather than leaving you guessing; if it simply cannot reach the access point, it
keeps trying and the message says that instead. See
[NETWORK_ARCHITECTURE.md](NETWORK_ARCHITECTURE.md) §6.3.

Keep in mind what that file is: a plain text file on your calculator containing
a Wi-Fi password. The shipped template keeps the section commented out, so a
fresh build carries nothing; if you do fill it in, do not commit the result, and
prefer a guest-network password over the one you use everywhere else. Nothing
the emulator does with it is worse than that file: it is sent only over the
docked serial link to the attached ESP32, which holds it in RAM rather than
flash, and both the calculator and the bridge wipe or drop it as soon as they
are done with it.

**Read the caveats.** The link is 115 200 baud ≈ 11.5 KiB/s shared between audio
and network, so audio is 8 kHz mono 8-bit and network throughput is modest. The
dock UART's register base is not part of the Ndless API and is unverified; the
port is opt-in via `-DWINSPIRE_CXLINK_UART`. Without an ESP32 everything still
works — you simply get no sound and no network.

The Sound Blaster's DMA refill is serviced by the frontend either way, because
the calculator build compiles the ISA DMA step out of the CPU batch for speed.
That means a guest audio driver keeps making progress with no ESP32 attached
rather than waiting forever for a block interrupt that would never arrive.

---

## 7. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| "This build needs a color TI-Nspire CX" | You have a Clickpad/Touchpad (PL110, 4bpp gray). Not supported. |
| "Unsupported LCD layout" | A panel type other than the two 16bpp layouts. Please report the reported `scr_type`. |
| Message box about a missing file | A path in `winspire.ini.tns` is wrong, or the file is not in the same folder as `nspire95-cx.tns`. |
| "Invalid INI entry at line N" | Syntax error in the INI. Section and key names are case-sensitive. |
| "Not enough free RAM ..." | Lower `mem_size` in the INI. |
| Display is rotated sideways on a CX II / rev W+ panel | Rebuild with `-DWINSPIRE_PANEL_ROTATE_CCW` (or without it, to flip back). The panel mount direction varies by hardware revision and cannot be detected at runtime. This only chooses a rotation direction; it cannot correct a mirror. |
| Display is mirrored, or unreadable | Fixed as of **v1.0.4** — the original CX presents the surface rotated 180°, so the frontend now rotates the guest surface by 180° before presenting it. Update if you are on v1.0.3 or older; v1.0.2/v1.0.3 corrected only the vertical axis, which on this panel leaves a left-right mirror. |
| Screen goes black after launching | Press a key to exit, then run the `DEBUG` profile and check the message box; DEBUG traces far more. |
| Guest never reaches the desktop | Check the image boots on a desktop emulator first. Windows 95 needs a matching IDE controller and `fill_cmos = 1`. |
| Very slow | Expected on the original CX. Try TURBO, raise `clock_hz` only if the guest complains about timers, and see PERFORMANCE.md for where the time actually goes. |
| No sound | Expected without an ESP32. With one: check `-DWINSPIRE_CXLINK_UART`, the UART base, and the audio counters in `cxlink_get_status()`. |
| "Audio bridge: N PCM blocks sent, M dropped" on exit | Either the link was up and went away while audio was playing, or the emulator could not keep up with the mixer and dropped PCM. Silence on a healthy setup — that message only appears when one of those happened. |
| No network | Expected without an ESP32. With one: check the `[network]` section in `winspire.ini` (a missing or rejected one is reported at startup and at exit), then the UART base and `-DWINSPIRE_CXLINK_UART` (NETWORK_ARCHITECTURE.md §6.3). |
| "Wi-Fi: [network] entry was not sent — ..." | The SSID or password in `winspire.ini` is out of range (ssid 1-31 characters, password 8-63). Fix the file and relaunch. |
| "The access point refused the credentials" on exit | The ESP32 associated far enough to be rejected: almost always a wrong password or an SSID that does not match. |
| Emulator hangs at startup | Power off, remove `disk.img.tns`, and retry: a corrupt image can wedge the IDE path. |

---

## 8. Repository map

```
source/winspire/          emulator core (shared by all targets)
  i386.c  fpu.c           CPU: real mode, protected mode, paging
  pc.c  pc.h              machine assembly, I/O dispatch, mixer
  vga.c  ide.c            video and storage
  i8042.c  i8259.c  i8254.c  i8257.c   input, interrupts, timers, DMA
  sb16.c  adlib.c  pcspk.c  fmopl.c    audio
  ne2000.c                Ethernet NIC (backend selected in net_open)
  cxlink.h  cxlink.c      CX <-> ESP32 protocol, bridge and Wi-Fi provisioning
  bridge_net.h  bridge_net.c  guest DHCP server + frame classifier,
                          shared with the ESP32 firmware and run by `make selftest`
  native.ini.tns          default winspire.ini (incl. the [network] section)
  cx_profiles.h           DEBUG / RELEASE / TURBO           [new]
source/winspire-ndless/   the calculator frontend (ported to the CX)
source/host/              headless host frontend + benchmark  [new]
source/bench/             x86 benchmark boot payload          [new]
source/ndless-stub/       Ndless API stub for type-checking   [new]
source/esp32/             ESP32 bridge firmware + build.sh    [new]
source/build-scripts/     build_host.sh  build_bench.sh  build_cx.sh
                          check_cx_frontend.sh                [new]
FREEBUFF_RECON.md         CX II dependency audit
CX_PORT_STATUS.md         what is verified vs not
PERFORMANCE.md            benchmark method and measured results
HARDWARE.md               CX hardware, dock connector, ESP32 wiring
AUDIO_ARCHITECTURE.md     Sound Blaster -> ESP32 -> speaker
NETWORK_ARCHITECTURE.md   NE2000 -> cxlink -> ESP32 -> Wi-Fi
```

## 9. Licence

`nspire95-cx` / WiNspire is BSD-3-Clause; third-party components retain their own
licences (see `LICENSES/`, including `tiny386-BSD-3-Clause.txt`). Windows 95 and
BIOS images are **not** distributed with this project.
