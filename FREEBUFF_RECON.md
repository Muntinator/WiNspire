# FREEBUFF_RECON.md — CX II dependency audit and CX port reconnaissance

Step 1 of the port brief was: *"First inspect the existing nspire95/Tiny386
source and identify every CX II dependency."*

This document is that audit. Every claim here was checked against the sources in
this repository, the Ndless SDK sources, or the published hardware documentation
(Hackspire, Datamath). Where something could not be verified, it says so
explicitly rather than guessing.

---

## 0. The premise correction that shapes the whole port

The brief asks to remove **ARMv7/Cortex-specific optimisations** and **CX II
APIs**. The first half of that turns out not to apply, and knowing why changes
the plan:

**The CX II is not ARMv7 and not Cortex. It is an ARM926EJ-S, the same core as
the original CX.**

- Datamath's CX II hardware entry and the reverse-engineering write-up
  (`zephray.me`, "On the way to overclock the TI-Nspire CX II") both describe the
  CX II as an ARM926EJ-S at 396 MHz with 64 MB of LPDDR.
- Hackspire's CPU table lists the same ARM926EJ-S (16 KiB I-cache, 8 KiB
  D-cache) across the Nspire family, changing only the SoC vendor.
- The existing build script already compiles with
  `-marm -mcpu=arm926ej-s -mtune=arm926ej-s`, which is only sensible *because*
  both targets share the core.

So this was never an ISA migration. The real gap between the CX II and the
original CX is:

| Axis | Original CX | CX II | Consequence for the port |
|---|---|---|---|
| Core | ARM926EJ-S | ARM926EJ-S | Instruction selection is already correct |
| Core clock | ~130–160 MHz | 396 MHz | **The** binding constraint: roughly 2.5–3x less guest throughput |
| SDRAM | 64 MiB | 64 MiB | Guest RAM ceiling is unchanged |
| Panel | 320x240, or 240x320 from revision W | 240x320 | New: the panel may be *rotated* on the CX |
| LCD controller | ARM PrimeCell PL111 | newer controller | Hardware cursor register is CX II only |
| Dock connector | present (J01, UART + GPIO) | see §4 | The only general-purpose external I/O |
| `set_cpu_speed()` | works | returns 0 | CX has a supported overclock path |

The takeaway: the port is about a **throughput deficit and a panel/cursor API
difference**, not an ISA difference. Optimisation carries most of the weight.

---

## 1. Hard CX II gates

### 1.1 The frontend refuses to run at all — `source/winspire-ndless/main.c`

```c
if (!is_cx2) {
        refresh_osscr();
        show_msgbox("WiNspire", "This build targets the CX II.");
        return 1;
}
```

This is the primary gate. `is_cx2` is a libndls macro defined as
`nl_hwsubtype() == 2`. The hardware subtype scheme, confirmed against the Ndless
`libndls.h`:

| Predicate | Meaning |
|---|---|
| `is_classic` (`hwtype() < 1`) | Clickpad/Touchpad, grayscale PL110, 4bpp |
| `hwtype() >= 1`, `!is_cm`, `!is_cx2` | **the original CX / CX CAS / CM-C family** |
| `is_cm` (`nl_hwsubtype() == 1`) | TI-Nspire CM-C |
| `is_cx2` (`nl_hwsubtype() == 2`) | CX II |

Note the consequence: the *original CX* is `hwtype() >= 1` with
`nl_hwsubtype() == 0`. It is neither `is_cx2` nor `is_cm`, so any code that
tests "is it an Nspire color model" via `is_cx2` is wrong for the CX.
**Status: replaced** by `detect_hardware()` (§3.1).

### 1.2 The LCD layout check is too narrow

```c
if (screen_format != SCR_320x240_565) {
        refresh_osscr();
        show_msgbox("WiNspire", "Unsupported LCD layout.");
        return 1;
}
```

Hackspire's screen table is unambiguous:

| Device | Screen | Controller |
|---|---|---|
| TI-Nspire Clickpad/Touchpad | 320x240, 4bpp greyscale | PL110 |
| TI-Nspire CX *(before revision W)* | 320x240, 16bpp colour | PL111 |
| **TI-Nspire CX *(from revision W)*** | **240x320, 16bpp colour — "LCD is rotated"** | same |

So a large fraction of shipped original-CX units report
`SCR_240x320_565`, not `SCR_320x240_565`, and the current frontend rejects them
with "Unsupported LCD layout". This is a genuine CX II assumption — the CX II
happens to always present a landscape view — and it is the single most likely
reason a naive build would fail on real CX hardware.

**Status: replaced** by a panel abstraction that accepts both 16bpp layouts and
rotates the guest surface when required (§3.2).

### 1.3 A raw CX II LCD register is written on every draw

```c
#define CURSOR_REG 0xC0000C00U
...
static void disable_os_cursor(void)
{
        volatile uint32_t *cursor_reg = (volatile uint32_t *)CURSOR_REG;
        *cursor_reg &= ~1U;
}
```

`0xC0000C00` appears nowhere in the Ndless API. It is a CX II LCD-controller
register being poked directly, and it is written before *every* LCD claim and
blit. On the original CX the PL111 register map at `0xC0000000` is different, so
this is not merely useless — it writes an arbitrary register in the display
controller's address space.

**Status: gated** behind `hw.has_hw_cursor`, which is true only for the CX II
(§3.3).

---

## 2. Build-system and configuration assumptions

### 2.1 `-mcpu=arm926ej-s` — correct, keep it

Already present in `build_native.sh`. Keep it and add `-mtune=arm926ej-s` (also
already present). This is the guarantee that no ARMv6/ARMv7-only instruction can
appear in the output, and it is now *enforced* by the new `build_cx.sh`, which
never passes a different `-march`/`-mcpu`.

### 2.2 The genzehn notice and toolchain flags

`--ndless-min 42 --ndless-rev-min 2004 --uses-lcd-blit 1`:

- `lcd_blit` is part of the modern libndls API (`void lcd_blit(void *buffer,
  scr_type_t buffer_type);`) and is available on the CX, so `--uses-lcd-blit 1`
  is fine to keep.
- `assert_ndless_rev(2004)` in the frontend is likewise satisfiable on the CX
  family, since Ndless 4.x supports OS 4.x on the CX as well as the CX II.

### 2.3 The "CX II" branding and the CX II-only clock path

- The genzehn `--notice` string said "x86 emulator for the TI-Nspire CX II".
  Cosmetic, but it is the string a user reads on the calculator.
- `source/nspire/clock_restore_arm.c` (used by the *Server* build, not the
  native build) drives the PMU at `0x90140030` with the value `0x21020303` for
  the CX II's 396 MHz stock clock. That is a CX II clock register and is not
  used on the CX. The CX instead has the supported `set_cpu_speed()` path — see
  §3.4.

### 2.4 Guest RAM ceiling

`GUEST_RAM_MAX` is 32 MiB and the default is 16 MiB. The CX has 64 MiB of
SDRAM, the same as the CX II, and the Ndless heap is what constrains the
emulator, not the installed RAM. The ceiling is therefore already
hardware-appropriate and was not changed; the CX-specific cost added by this
port is at most one extra 153,600-byte panel buffer (§3.2), and only on rotated
units.

---

## 3. What the port does about each finding

### 3.1 `detect_hardware()` replaces the CX II gate

Probes `is_classic`, `is_cx2`, and `lcd_type()`, and fills a `NspireHardware`
struct. The grayscale PL110 models are rejected with an accurate message
("needs a color TI-Nspire CX"), because a 4bpp panel genuinely cannot display
the 16bpp guest surface — that is a hardware fact, not a policy choice.

### 3.2 Panel abstraction with rotation

`panel_blit()` presents the 320x240 guest surface:

- **320x240 panel** (CX before revision W): straight `lcd_blit(fb,
  SCR_320x240_565)`, exactly as before.
- **240x320 panel** (CX from revision W, and CX II): the frame is transposed
  into a dedicated 240x320 buffer, then blitted as `SCR_240x320_565`.
- The partial-update path (`draw_region`, which `memcpy`s straight into
  `REAL_SCREEN_BASE_ADDRESS`) is disabled on rotated panels, because that
  surface strides by the *panel* width and cannot be patched with 320-wide
  rows.

The rotation direction is a compile-time choice (`WINSPIRE_PANEL_ROTATE_CCW`)
because the panel mount direction varies by revision and cannot be determined
from the Ndless API. This is called out as needing verification on hardware.

### 3.3 Hardware cursor is CX II only

`disable_os_cursor`, `hide_os_cursor` and `restore_os_cursor` all become no-ops
unless `hw.has_hw_cursor` is set. The original CX therefore never touches
`0xC0000C00`.

### 3.4 The CX gets the supported overclock path

This is the one place where the original CX is *better* than the CX II:

```c
/* libndls set_cpu_speed(): returns 0 immediately on the CX II. */
if (hw.can_set_cpu_speed) {
        unsigned previous = set_cpu_speed(CPU_SPEED_150MHZ);
        saved_cpu_speed = previous;
        cpu_speed_changed = previous != 0 && previous != CPU_SPEED_150MHZ;
}
```

`set_cpu_speed()` writes `0x900B0000` on the CX-family SoC. The previous value
is restored at exit, because leaving the calculator overclocked after returning
to TI-OS is not acceptable. This is free throughput that the CX II cannot use,
and it is a supported Ndless API rather than a PMU hack.

---

## 4. The dock connector — what is actually available for the ESP32 bridge

The brief says: *"Do not invent unsupported CX I/O capabilities; inspect the
actual CX/Ndless interface and design around what is really available."*

### 4.1 What the hardware has

Hackspire documents the dock connector (silkscreen **J01**) on the Nspire line:

| Signal | Pin |
|---|---|
| Vcc | 2 |
| Rx | 3 |
| Tx | 4 |
| GND | 5 (long pin) |
| GPIO4 / USB Data+ with Navigator cradle | 6 |
| USB Data- with Navigator cradle | 7 |
| GPIO0 | 17 |
| GPIO22 | 18 |

and states plainly: *"A serial adapter can be connected to the Tx/Rx/GND pins of
the dock connector… Warning: only use TTL voltage levels, never RS232 levels."*
with **115200 8N1, no flow control**. This is a real, documented, bidirectional
TTL UART — the right physical link for the ESP32.

The same page records that the dock connector's availability varies by board
revision (the related **J04** debug connector was removed and later returned on
revision W boards), so a given unit should be checked before assuming the pads
are populated.

### 4.2 What Ndless exposes

**Nothing for the dock UART.** There is no UART, GPIO, or dock API in
`libndls.h`. The exported surface relevant here is:

- `lcd_init` / `lcd_blit` / `lcd_type`, `hwtype()`, `nl_hwsubtype()`
- `set_cpu_speed`
- `touchpad_scan` / `touchpad_getinfo` / `is_touchpad` / `is_keyPressed`
- `usbdi.h` — the CX's USB port as a **device** (descriptors, endpoints), not a
  host and not a bulk pipe to an arbitrary peripheral

So the ESP32 bridge has to drive the SoC UART by MMIO. That is the honest
situation, and it is why `cxlink.c` puts the UART behind a small HAL with the
register base as an overridable constant
(`WINSPIRE_CXLINK_UART_BASE`, default `0x90040000`) and makes it **opt-in** via
`-DWINSPIRE_CXLINK_UART`.

**Unverified:** the CX SoC's UART register base is not part of the Ndless API
and this port has not confirmed it on hardware. With the flag off, `cxlink` runs
with no transport, which keeps the rest of the system testable and makes the
failure mode obvious (no frames move) instead of a hang.

### 4.3 What this means for the design

The bus is a **115200 8N1 link = 11520 bytes/s exactly.** That number drives
the whole protocol design, and it is why:

- Audio is fire-and-forget, drop-oldest, and rate/format negotiable.
- Network frames are the reliable, acknowledged channel.
- Neither side is ever allowed to block the other.

See `cxlink.h` for the bandwidth budget and `AUDIO_ARCHITECTURE.md` /
`NETWORK_ARCHITECTURE.md` for the consequences.

---

## 5. Confirmed non-issues

Checked and deliberately **not** changed:

- **Instruction set.** Nothing in the tree uses ARMv6/ARMv7/NEON/Cortex
  instructions; `-mcpu=arm926ej-s` already prevents it.
- **`lcd_blit` usage.** Part of the modern API and available on the CX.
- **Touchpad handling.** `is_touchpad` / `touchpad_scan` / `touchpad_report_t`
  exist for the CX's capacitive touchpad, and the existing deadzone/acceleration
  logic is model-independent.
- **Guest RAM sizing.** 16 MiB default / 32 MiB max is appropriate for a 64 MiB
  machine on both models.
- **Server build.** `source/server/*` targets LinuxLoader2 on the CX II and is
  out of scope for a native CX port; it was left untouched.

---

## 6. Summary table

| # | Finding | Severity | Disposition |
|---|---|---|---|
| 1.1 | `if (!is_cx2) return 1;` hard gate | blocker | replaced by `detect_hardware()` |
| 1.2 | `SCR_320x240_565`-only LCD check rejects rotated CX panels | blocker | panel abstraction + rotation |
| 1.3 | Raw `0xC0000C00` CX II cursor register written every draw | correctness | gated on `has_hw_cursor` |
| 2.1 | `-mcpu=arm926ej-s` already correct | none | enforced in `build_cx.sh` |
| 2.2 | `--uses-lcd-blit` / `assert_ndless_rev(2004)` | none | confirmed valid for CX |
| 2.3 | CX II PMU clock value in the Server helper | none (server-only) | untouched; CX uses `set_cpu_speed()` |
| 2.4 | Guest RAM ceiling vs 64 MiB SDRAM | none | unchanged |
| 4.2 | No Ndless API for the dock UART | risk | MMIO HAL, opt-in, documented as unverified |
