# HARDWARE.md — TI-Nspire CX hardware and ESP32 wiring

Facts here are sourced from Hackspire (the TI-Nspire hacking wiki), Datamath's
calculator museum, and the TI-Nspire series specifications. Anything that is an
assumption rather than a documented fact is marked **ASSUMPTION**.

---

## 1. The original TI-Nspire CX

| Property | Value | Note |
|---|---|---|
| SoC | Texas Instruments ET-NS2010B | Toshiba ASIC |
| Core | ARM926EJ-S | ARMv5TE, Jazelle |
| Clock | ~130–160 MHz | sources differ by revision; **the CX II is 396 MHz** |
| I-cache | 16 KiB | |
| D-cache | 8 KiB | |
| SDRAM | 64 MiB | Samsung K511F12ACA before rev J, ESMT FM60D1G12A from rev J |
| NAND | 128 MiB | shared package with SDRAM; no execute-in-place |
| NOR | internal (512 KiB from rev W) | boot1 + certificate memory |
| Display | see below | |
| MMU | enabled | remaps NOR/RAM/peripherals and controls cache attributes |

### 1.1 Display — the part that breaks naive ports

| Device | Panel | Controller | Orientation |
|---|---|---|---|
| Clickpad / Touchpad | 320x240, 4bpp greyscale | ARM PrimeCell **PL110** | landscape |
| CX *(before revision W)* | 320x240, 16bpp colour | ARM PrimeCell **PL111** | landscape |
| **CX *(from revision W)*** | **240x320, 16bpp colour** | (same family) | **rotated** |
| CX II | 240x320, 16bpp colour | newer controller | rotated |

Hackspire states it plainly for revision W: *"New LCD + J04 connector back +
incompatible versions < 4.0.1"*, and the screen table adds *"LCD is rotated"*.

Consequences for this port, all implemented in `source/winspire-ndless/main.c`:

- `lcd_type()` may return `SCR_320x240_565` **or** `SCR_240x320_565`. Both are
  accepted; the guest surface stays 320x240 and is transposed for a rotated
  panel.
- The 4bpp grayscale models cannot show a 16bpp surface, and are rejected with
  an accurate message.
- The CX II's hardware-cursor register at `0xC0000C00` is **not** part of the
  PL111 map, so it is never written on a CX.
- **ASSUMPTION:** the rotation direction for a 240x320 panel. It varies by
  mount and cannot be read from the Ndless API. `WINSPIRE_PANEL_ROTATE_CCW`
  selects it and must be set empirically on hardware. This is a rotation
  choice only.
- **MEASURED PER UNIT, NOT ASSUMED:** the orientation correction applied on the
  non-rotated path. It is read from `winspire.ini` under `[nspire]` as
  `orientation` (0 none, 1 flip top-bottom, 2 flip left-to-right, 3 rotate
  180) and defaults to 3. It cannot be derived from the Ndless API, and four
  releases of guessing at it went wrong, so it is set per unit instead. Set
  `orientation_marker = 1` and one photograph of the corner brackets
  identifies the right value; see `CX_PORT_STATUS.md` §4b.
  `transform_surface()` in `source/winspire-ndless/main.c` implements it. A
  transposed panel does not use this, which is why it is keyed on
  `!rotated_panel`.
- Note that the TI-Nspire OS's bottom-left drawing origin is **not** evidence
  about fill order: it describes how the OS issues drawing commands, not the
  order in which `lcd_blit()` writes panel memory. An earlier version inferred
  a bottom-up scan order from it and corrected only the vertical axis; that is
  the mistake that produced the mirror. See `CX_PORT_STATUS.md` §4b.

### 1.2 CPU speed control

| Register | Used by | Effect |
|---|---|---|
| `0x900B0000` / `0x900B000C` | libndls `set_cpu_speed()` | CX-family clock control; returns 0 (no-op) on CX II |
| `0x90140030` | PMU, used by `source/nspire/clock_restore_arm.c` | CX II: `0x21020303` = 396 MHz stock, `0x29020303` = 492 MHz |

The original CX therefore has a **supported** overclock path
(`set_cpu_speed(CPU_SPEED_150MHZ)`, where `CPU_SPEED_150MHZ == 0x00000002`), and
the port uses it, restoring the previous value on exit. The CX II has to use the
PMU value instead. Because the CX's core clock is roughly a third of the CX II's,
this is a meaningful chunk of the performance gap.

---

## 2. Connectors

### 2.1 Dock connector — J01 (the ESP32 link)

Pin 1 is at the left-hand side with the calculator upside down and the connector
at the bottom.

| Signal | Pin | Notes |
|---|---|---|
| Vcc | 2 | |
| **Rx** | 3 | calculator receives |
| **Tx** | 4 | calculator transmits |
| **GND** | 5 | long pin |
| GPIO4 / USB Data+ (with Navigator cradle) | 6 | |
| USB Data- (with Navigator cradle) | 7 | |
| Vin | 8 | medium pin |
| GPIO0 | 17 | |
| GPIO22 | 18 | |
| Vin | 19 | medium pin |
| GND | 22 | long pin |

Hackspire on the UART:

> A serial adapter can be connected to the Tx/Rx/GND pins of the dock connector
> … **Warning: only use TTL voltage levels, never RS232 levels.**
> 115200, 8 data bits, no parity, 1 stop bit, no flow control.

**This is the physical link cxlink uses.** 115200 8N1 is exactly
11 520 bytes/s and that number drives the protocol budget — see `cxlink.h`.

Two cautions:

- Connector population varies by revision. The related debug connector **J04**
  was removed and later restored on revision W; do not assume J01 pads are
  populated on a given unit.
- **ASSUMPTION:** that the SoC UART driving these pins is reachable by MMIO and
  at which base. Ndless exposes **no** UART API — `libndls.h` has `lcd_*`,
  `set_cpu_speed`, `touchpad_*`, `isKeyPressed` and `usbdi`, nothing for UART or
  GPIO. `cxlink.c` therefore drives the UART directly and defaults to
  `0x90040000` (`WINSPIRE_CXLINK_UART_BASE`), by analogy with the classic
  Nspire's UART. **This must be confirmed on hardware before the bridge is
  trusted.**

### 2.2 USB

The CX's USB port is a **device** port. Ndless exposes it through `usbdi.h` as
device-side descriptors and endpoints, not as a host controller and not as a
general bulk pipe to an arbitrary peripheral. This is why the ESP32 bridge is a
UART link rather than USB: USB would be far faster, but it is not usable for this
without a custom USB device implementation on both ends.

### 2.3 J04 debug connector

A 30-pin header footprint. Notably it exposes RS232-level signalling
(115200 8N1) for the boot ROM's logging and X-Modem recovery, and several
GPIOs. It is a debug interface, generally unpopulated, and is **not** used by
this port.

---

## 3. Wiring the ESP32

### 3.1 Minimum viable link

| TI-Nspire CX (dock J01) | ESP32 | Notes |
|---|---|---|
| pin 4 — Tx | GPIO16 (`CXLINK_UART_RX_PIN`) | calculator → ESP32 |
| pin 3 — Rx | GPIO17 (`CXLINK_UART_TX_PIN`) | ESP32 → calculator |
| pin 5 — GND | GND | **common ground is mandatory** |

Cross Tx/Rx. Both sides are 3.3 V TTL, so no level shifting is required.

The ESP32 is a 3.3 V part and the calculator's signalling is 3.3 V TTL, so the
UART lines can be connected directly. **Do not** power the ESP32 from dock pin 2
(Vcc) without checking that board's current budget — use its own supply and tie
grounds.

### 3.2 Audio

| ESP32 | Peripheral |
|---|---|
| GPIO26 (`CXLINK_I2S_BCLK_PIN`) | I2S bit clock |
| GPIO25 (`CXLINK_I2S_WS_PIN`) | I2S word select |
| GPIO22 (`CXLINK_I2S_DOUT_PIN`) | I2S data out → DAC (e.g. MAX98357A, PCM5102) |

Pin numbers are `#define`s at the top of `source/esp32/main/main.c` and should be
changed to match the board.

### 3.3 Grounding and noise

The dock UART shares a ground with the calculator's digital section. Long
unshielded leads at 115200 baud are generally fine, but if `crc_errors_last`
climbs in `cxlink_get_status()` under load, shorten the leads before suspecting
the protocol — CRC drops are reported rather than silently retried, precisely so
a marginal link is visible.

---

## 4. Guest-visible hardware (emulated)

The point of the brief's non-negotiable principle is that Windows 95 must be
made usable by *optimising*, not by removing hardware. This is the complete set
of emulated devices the guest sees. Nothing here is optional or disabled in any
profile, including TURBO:

| Class | Emulated | Source |
|---|---|---|
| CPU | i386: real mode, 32-bit protected mode, paging, TSS/task switching, interrupts, exceptions, I/O and string instructions, `rep` | `i386.c`, `fpu.c` |
| Chipset | Intel 440FX PCI host bridge, port 92 fast reset | `pci.c`, `pc.c` |
| Interrupts | 8259A PIC pair (master + slave, cascade) | `i8259.c` |
| Timers | 8254 PIT (channels 0–2), CMOS/RTC IRQ | `i8254.c`, `misc.c` |
| DMA | 8257/8237 ISA DMA controllers (8- and 16-bit) | `i8257.c` |
| Input | 8042 PS/2 controller, PS/2 keyboard, PS/2 mouse | `i8042.c` |
| Storage | IDE/ATA controller, master + secondary, persistent disk image | `ide.c` |
| Graphics | VGA with its own BIOS, text and graphics modes, palette, VRAM | `vga.c`, `vgabios.bin` |
| Audio | Sound Blaster 16: DSP, I/O registers, DMA, IRQ, PCM | `sb16.c` |
| Audio | Adlib/OPL2 FM synthesis | `adlib.c`, `fmopl.c` |
| Audio | PC speaker | `pcspk.c` |
| Network | NE2000-compatible Ethernet adapter, MAC, TX/RX, IRQ | `ne2000.c` |
| Firmware | SeaBIOS-derived BIOS image | `bios.bin` |

Windows 95 has in-box drivers for the NE2000 and the Sound Blaster 16, which is
why those two were chosen over anything more exotic.

### 4.1 Network backend

The NE2000's *backend* is where the ESP32 plugs in. The existing code already
supports tuntap, slirp, and a null backend (`source/winspire/ne2000.c`); this
port adds a `USE_CXLINK` backend following the same pattern. The guest-facing
card is byte-for-byte unchanged, which is what "the guest must not need to know
that an ESP32 is providing the physical network connection" requires.

---

## 5. Quick hardware checklist for a first run

1. Confirm the unit is a **colour** Nspire (CX / CX CAS / CX II), not a
   grayscale Clickpad/Touchpad.
2. Note the hardware revision — a 240x320 panel means revision W or later, and
   `WINSPIRE_PANEL_ROTATE_CCW` may need flipping. The original CX needs no
   such choice: its 180-degree rotation is handled in the frontend.
3. Check J01 is populated before soldering an ESP32 to it.
4. Have Ndless installed and note its revision (`assert_ndless_rev(2004)`).
5. Have `disk.img.tns`, `bios.bin.tns`, `vgabios.bin.tns` and
   `winspire.ini.tns` alongside `nspire95-cx.tns` on the calculator.
