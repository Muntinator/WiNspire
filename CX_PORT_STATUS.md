# CX_PORT_STATUS.md — where the original-CX port actually stands

The brief's definition of done is a calculator running Windows 95. This document
is deliberately blunt about how far the repository gets toward that, because the
difference between "written and type-checked" and "observed working on a
calculator" is the whole difficulty of a port like this.

**Nothing in this repository has been run on a TI-Nspire CX.** There is no
calculator in the environment where this work was done. The Ndless toolchain
was assembled here (see §9), so real `.tns` binaries now exist and are
validated structurally - but nothing has executed them. Every claim below is
labelled with how it was verified.

---

## Verification legend

| Label | Meaning |
|---|---|
| **verified** | Executed in this workspace and the result was observed |
| **builds clean** | Compiled with the real cross-toolchain for its target; no errors or warnings. Says nothing about runtime behaviour |
| **typechecked** | Compiled with the host compiler against the real Ndless API shapes (`check_cx_frontend.sh`) |
| **inspected** | Established by reading the sources; no build or execution |
| **unverified** | Written but not compiled or run anywhere; needs hardware or a toolchain |

---

## Work completed

### 1. CX II dependency audit — **verified** (by inspection of sources and the
Ndless SDK)

Every CX II dependency in the tree is enumerated with its disposition in
[FREEBUFF_RECON.md](FREEBUFF_RECON.md). The important finding is that the CX II
and the original CX share the same ARM926EJ-S core, so this is not an ISA port;
it is a throughput and panel-API port.

### 2. Frontend port (`source/winspire-ndless/main.c`) — **typechecked**

- `detect_hardware()` replaces the `is_cx2` hard gate and the
  `SCR_320x240_565`-only LCD check.
- Panel abstraction accepts both 16bpp layouts. Rotated 240x320 panels
  (CX revision W and later, and CX II) get a transposing `panel_blit()`; the
  linear partial-update path is disabled on them.
- The raw CX II cursor register `0xC0000C00` is now touched only when
  `hw.has_hw_cursor` is set, so the original CX never writes it.
- Grayscale PL110 models are rejected with an accurate message.
- The CX's supported `set_cpu_speed(CPU_SPEED_150MHZ)` path is used where it
  works, with the previous value restored at exit.

### 3. Ndless API type-check harness — **verified**

`source/ndless-stub/` mirrors the declared Ndless API (`hwtype`,
`nl_hwsubtype`, `is_classic`/`is_cm`/`is_cx2`, `scr_type_t` and the `SCR_*`
values, `lcd_*`, `set_cpu_speed` and `CPU_SPEED_*`, `touchpad_*`, `isKeyPressed`,
`t_key`/`tpad_arrow_t`, `REAL_SCREEN_BASE_ADDRESS`), copied from the SDK sources.

`source/build-scripts/check_cx_frontend.sh` compiles the CX frontend and every
core translation unit against it. Observed result, all three profiles:

```
ok   main.c … ok   cxlink.c, ok   bridge_net.c     (18 translation units, three profiles)
CX frontend + core: type-check clean (DEBUG|RELEASE|TURBO profile)
```

`bridge_net.c` is not compiled into the `.tns` — it runs on the ESP32 — but it is
shared source, it is freestanding, and checking it here is what keeps every shared
translation unit ARM-clean with one command.

This caught two real defects during development (a struct field named `is_cx2`
colliding with the libndls macro, and a use-before-declaration of `panel_blit`),
which is exactly its purpose.

### 4. Host build + benchmark infrastructure — **verified**

- `source/host/main.c` — headless frontend that builds the *same* core with the
  *same* `WINSPIRE_NATIVE_BUILD` + `release_config.h` configuration as the
  calculator target.
- `source/bench/bench386.S` — a deterministic real-mode x86 benchmark payload
  with eight measured instruction-mix phases, booted by the emulated BIOS.
- `source/build-scripts/build_host.sh`, `build_bench.sh` — build and assemble.
- `source/winspire/cx_profiles.h` + `build_cx.sh` — DEBUG / RELEASE / TURBO
  profiles that change diagnostic and presentation work only, never emulated
  hardware.

Measured: **~80 M emulator steps/s** steady state, reproducible within ~4%.
Full numbers in [PERFORMANCE.md](PERFORMANCE.md).

The harness also has a `--screen` mode that dumps the emulated text screen
straight out of VRAM. It exists so that a change to the renderer can be proved
output-identical without a calculator; it is what the text-refresh optimisation
in [PERFORMANCE.md](PERFORMANCE.md) §3.4 was verified with.

The loop also drives the PC audio path (`pc_audio_step()`) and the ESP32 bridge
the same way the calculator frontend does, and reports both. See §7.

### 4a. Text-mode refresh — **optimised on measurement**, output verified
**identical**

The VGA device path was the largest term in the host profile at 74–83% of wall
time. It was being spent re-examining all 2000 character cells of a text screen
on every retrace poll to discover that none of them had changed: 1.2 billion cell
comparisons for 604 000 dirty cells, and the cursor cell re-blitted on every
poll instead of on every blink.

`source/winspire/vga.c` now skips the scan entirely when a counter bumped by
every VRAM store and every VGA/VBE register write says no pixel can differ, and
caches the cursor cell like any other. Measured over three runs each way:
redraw regions 842 220 -> **25**, `vga step` 74.3% -> **16.6%** of wall, and
**4.35x more poll cycles per second** during BIOS POST (where the guest is
halted and those cycles are what re-arm the PIT and deliver the timer IRQ). The
headline `instructions/sec` did not move and is not claimed to have: POST is a
fixed 2.6 s timer wait. The rendered SeaBIOS POST screen and its snapshot
signature are byte-identical before and after; all three profiles build
warning-free and `check_cx_frontend.sh` is clean.

### 4b. Display orientation — **now a setting**, correctness **verified**, value **still unread on hardware**

The panel correction is no longer compiled in. It is read from `winspire.ini`:

    [nspire]
    orientation = 3      ; 0 none, 1 flip top-bottom, 2 flip left-right, 3 rotate 180
    orientation_marker = 1

`orientation_marker = 1` draws white brackets in the four corners with four
different pairs of leg lengths, so **one photograph identifies which corner is
which** and therefore exactly which setting the unit needs. Turn it off
afterwards; it is an overlay, not part of the guest screen.

#### Why this stopped being a constant

Four releases have now shipped a different orientation guess, and the fifth
guess cannot be made from here either: no calculator is available, and the
reports available in this workspace do not unambiguously separate the four
possibilities. Guessing again would be the same mistake a fifth time. The
property that determines the answer is cheap to measure on hardware and
impossible to deduce from the Ndless API, so it is now measured.

The evidence that did *not* survive scrutiny is worth recording. v1.0.2
inferred a bottom-up scan order from the TI-Nspire OS's bottom-left drawing
origin. That origin describes how the OS issues drawing commands
(`screen.drawString`, `gui_gc_fillRect`); it says nothing about the order in
which `lcd_blit()` fills panel memory. Conflating them is what produced a
left-right mirror where a vertical flip had been intended. Two further claims
recorded earlier also did not hold up and should not be repeated: that
`lcd_init(SCR_320x240_565)` was wrong, and that a CX II/rev W+ panel needed
different handling.

One useful negative result: the transform is applied identically to the BIOS
POST screen and to Windows 95, and both were wrong in the same way. That rules
out the guest, the VGA BIOS and the resolution - the fault is in the blit,
which is the one place this code can fix.

#### Verified here

`transform_surface()` and the `draw_region()` staging loop are the same code
for all four settings, so the invariant that matters is that **the partial
update path agrees with the full-frame blit** for every setting - if they
disagree the screen tears into a mixture of orientations and freezes stale
pixels, which is the v1.0.3 defect. An index-level harness establishes the
full-frame result and then asserts, pixel by pixel, that nine partial-update
bands leave exactly that result: **4 settings x 9 bands, 0 wrong pixels**.

The harness has teeth: reintroducing the v1.0.3 copy-direction bug makes it
**segfault**, the same out-of-bounds write that shipped as a real defect in
v1.0.2. It also caught two out-of-range bands while being written, which is
why `draw_region()` now clips its rectangle itself instead of trusting the
caller - every branch below computes panel coordinates by reflecting the guest
rectangle, so an unclipped one writes outside both the staging buffer and the
panel.

Also verified: type-check clean on all three profiles; all three `.tns` build
warning-free (337076 / 363964 / 383260 bytes) and pass `genzehn`; `make
selftest` PASS; the benchmark still completes, exit 0, 26 redraw regions,
`vga step` 16.6% of wall.

**Still open, and only the owner of the hardware can close it:** which of the
four values this particular CX needs. It is one edit to one line in
`winspire.ini` and no rebuild - deliberately.

### 5. cxlink protocol, Nspire-side bridge, and the guest DHCP server —
**typechecked**, with an **executable self test** for both

`source/winspire/cxlink.h` (shared wire format) and `cxlink.c` (framing,
CRC-16/CCITT, sequence numbers, reliable network channel with ACK/retry,
unreliable audio channel with drop-oldest, byte rings sized as powers of two
because the ARM926 has no divide instruction) plus the `USE_CXLINK` NE2000
backend in `ne2000.c`, which follows the existing tuntap/slirp pattern.

`source/winspire/bridge_net.h` and `bridge_net.c` are the network half of the
same shared source: the guest subnet, the DHCP server and the frame classifier
the ESP32 firmware uses, written freestanding (no lwIP, no ESP-IDF, no Ndless)
precisely so that the one part of the router that is hardest to debug on a real
Windows 95 guest can be executed here. They are compiled into both the host build
and the firmware.

`cxlink.c`, `bridge_net.c` and the modified `ne2000.c` are all in the type-check
run and build clean, and all three are compiled by the host build. `make selftest`
runs the protocol, audio framing, corruption-recovery and DHCP checks for real:

```
$ build/Host/winspire-host --selftest
cxlink self test: PASS (codec, ack/retry, audio framing, corruption recovery,
                       reset, wifi provisioning)
bridge_net self test: PASS (DHCP offer/ack/nak, lease expiry, release, decline
                       quarantine, checksums, frame routing)
```

The provisioning section of that suite is what makes the Wi-Fi path testable at
all: it drives credential validation over its boundary cases (10 of them,
including a 32-character SSID and a 64-character password, both of which are
refused because ESP-IDF's fields must stay NUL-terminated), asserts the exact
`NET_CONFIG` frame the bridge receives byte for byte, asserts that the
retransmission stops once the bridge confirms, that it resumes when the bridge
reports losing the credentials (a restart), and that the stored password is
really zeroed when the frontend clears it.

The DHCP test is what makes the routing backend more than "it compiles": DHCP
replies are built, checksum-validated and classified entirely in memory, and the
suite found three real defects while it was written (an IP checksum that skipped
four bytes instead of two, a REQUEST naming an unused address being re-pointed at
a different lease, and a frame padded to the Ethernet minimum being rejected).
Two deliberate mutations (removing the existing-lease lookup; freeing instead of
quarantining on DECLINE) each made the suite fail at the expected check and were
then reverted, which is the only evidence available here that the test has teeth.

### 6. ESP32 firmware — a built network router, never run

`source/esp32/` contains the bridge firmware: cxlink framing over UART1, an I2S
audio path with a ring buffer and underrun accounting, and a complete IPv4 NAT
router for the guest — a DHCP server on the guest subnet, a raw lwIP netif the
guest's frames arrive on, lwIP forwarding with source NAT, and a Wi-Fi station
uplink. It is no longer "the frame plumbing for forwarding": `net_forward_ipv4()`
hands the frame to lwIP's input path, and the routing and translation are lwIP's
(`CONFIG_LWIP_IP_FORWARD` + `CONFIG_LWIP_IPV4_NAPT`), armed only on the guest
interface. The DHCP server itself is the shared, host-tested `bridge_net.c` of §5
rather than an lwIP app, for the reason given in NETWORK_ARCHITECTURE.md §6.2.

ESP-IDF **was** installed for this and the firmware **compiles clean**: ESP-IDF
v5.5.5 with xtensa-esp-elf-gcc 14.2.0, cold build from an empty tree, no errors
and no warnings, producing three images that `esptool image_info` validates
(checksum and validation hash both OK):

```
bootloader/bootloader.bin                 26128 bytes
partition_table/partition-table.bin        3072 bytes
cxlink-bridge.bin                       807296 bytes   (23% of the 1 MiB app partition free)
```

Rebuild with `bash source/esp32/build.sh` (or `make esp32`); it writes to
`build/esp32/`. Nothing has been *flashed*: there is no ESP32 and no calculator
here, so every runtime claim about this firmware remains unverified. What changed
in this pass, and what that costs in confidence, is spelt out below.

Two defects in this file were found by reading the calculator's decoder and by
counting the buffers, not by any test here — which is the useful lesson:

- The firmware wrote ACK sequence numbers big-endian while `cxlink.c` decodes them
  little-endian, so no ACK would ever have matched and every network frame would
  have been retransmitted until the link was declared down.
- The link task's stack was 4 KiB, which is smaller than one frame plus the
  dispatch buffers plus a DHCP reply plus the encoder output. It is 8 KiB now,
  and the arithmetic is in a comment next to the `#define`.

Both were compile-clean and would only have shown up on hardware, which is the
honest description of what "builds clean" buys on this side of the project.

The firmware also holds the receiving half of uplink provisioning: it validates
the credential pair with the same shared function the calculator uses, applies it
only when it differs from the pair in use, maps the Wi-Fi disconnect reason onto
`CXLINK_NET_STATE_REJECTED` or `CXLINK_NET_STATE_ASSOCIATING` so a wrong password
is distinguishable from an out-of-range access point, and reports both in every
`NET_CONFIG` (every 2 s). None of that has run either - see §6 of
NETWORK_ARCHITECTURE.md.

The first build of this file failed at link with undefined references to
`cxlink_encode` and `cxlink_decoder_push`, because `main/CMakeLists.txt` put
`source/winspire` on the include path but never compiled `cxlink.c`. That is
fixed by compiling the shared source into the firmware; it is also why the shared
protocol now has an executable test on the host side (`make selftest`).

### 7. Audio and bridge integrated into the frontend loop — **verified on the
host**, **typechecked** for the CX

This is the piece that turns the bridge from an interface into a working path.
Both frontends now service it from the main loop rather than from the guest
interpreter:

- `cxlink_poll()` is called once per millisecond of elapsed time (guest cycles on
  the calculator, the monotonic host clock here), which is also the video and
  input interval.
- The mixer output is routed to the link through `pc_audio_step()` (mixer pull +
  ISA DMA refill, in `pc.c`) and `cxlink_audio_resampler_feed()` (s16 stereo
  44100 -> mono s8 at the negotiated rate, framed in 128-sample blocks).
- `pc_audio_step()` runs unconditionally, even with no ESP32 attached, because the
  native build compiles the ISA DMA step out of `pc_step()` — without this pull a
  guest Sound Blaster driver would wait forever for its block interrupt.

The host run exercises that whole path against a real BIOS boot and the
benchmark payload (`--boot`), with a sink transport answering the handshake, and
reports it:

```
aio bridge         : link up, 23424 link samples, 25430 frame bytes to transport
wifi provision    : ssid "HomeNet" applied by the simulated bridge, 1 credential frame(s), confirmed, 1 keepalive(s) answered
```

The second line is the provisioning path running for real. The host link decodes
what the calculator writes (the same decoder both ends use) and behaves like the
bridge for two exchanges: it answers the keepalive, and it applies the credentials
a `NET_CONFIG` carries and reports them back as held. Everything on the
calculator's side of that - reading `[network]` from an INI, validating it,
framing the credentials, retransmitting until confirmed, then stopping - is the
shipped code. What a simulated bridge cannot tell you is whether the radio then
associates; see §6 above.

Those byte counts are the evidence the datapath is live: they can only be
non-zero if the mixer ran, DMA refilled, samples were resampled, frames were
encoded with CRCs and the TX ring drained. Note what this does **not** prove:
the guest never programs the Sound Blaster during a BIOS boot, so the mixer
output is silence (`0 non-silent`). Real audio content on the CX still needs
hardware — see [AUDIO_ARCHITECTURE.md](AUDIO_ARCHITECTURE.md).

### 8. Documentation — **verified** (it is the artefact)

`README_CX.md`, `FREEBUFF_RECON.md`, `CX_PORT_STATUS.md`, `PERFORMANCE.md`,
`AUDIO_ARCHITECTURE.md`, `NETWORK_ARCHITECTURE.md`, `HARDWARE.md`,
`source/esp32/README.md`.

### 9. Calculator binaries — **builds clean**, **byte-reproducible**; runtime
**unverified**

`nspire95-cx.tns` exists. The toolchain blocker is closed by
`source/build-scripts/setup_ndless_sdk.sh`, which assembles a working Ndless
SDK in about a minute out of Ubuntu's arm-none-eabi GCC 10.3 + newlib and the
Ndless sources (libndls, libsyscalls, the zehn loaders, genzehn) instead of
building upstream's binutils/gcc/newlib from source. Four environment
compatibility fixes are needed; each is marked and explained in the script
(newlib 3.3 losing `PATH_MAX` and wanting `_init`/`_fini`, binutils 2.38
rejecting a newer ldscript sort expression, and a php-based header
regeneration rule).

All three profiles build here with zero warnings (`make cx`, `make cx-release`,
`make cx-debug`):

```
nspire95-cx.tns          382168 bytes   TURBO
nspire95-cx-release.tns  362872 bytes   RELEASE
nspire95-cx-debug.tns    335960 bytes   DEBUG
```

(Sizes as of the v1.0.1 release. The text-refresh optimisation in §4a adds
~200–500 bytes per profile.)

What is verified about them:

- `genzehn --info` parses the zehn header inside each one: correct application
  name/author/notice, valid relocations, correct entry point.
- The package's own media boots: the shipped `winspire.ini.tns` together with
  `bench386.img.tns` runs on the workstation build of the *same core* and the
  benchmark payload completes (3.86 M instructions of BIOS POST first).
- **Byte-reproducible**: running `setup_ndless_sdk.sh` into a fresh directory
  and rebuilding produced a byte-identical `nspire95-cx.tns`.
- The first real link caught two defects that type-checking structurally
  cannot: the DEBUG profile calls `nspire_log()` and no calculator frontend
  defined it (the sink now lives in `source/winspire-ndless/main.c` and writes
  `winspire.log.tns` line by line), and GCC 10 could not prove the `COND()`
  switch in `i386.c` exhaustive (it is written so that it can).

What is **not** verified: execution on a calculator. A `.tns` that validates
and links can still hang at `lcd_init`, draw a rotated screen or crash in the
first `pc_step`; that is what the first hardware session is for. The DEBUG
build and the unstripped `nspire95-cx.elf` (kept in `build/CX/`) exist for
that session.

---

## Not done, and what it needs

| Item | Blocker |
|---|---|
| Running `nspire95-cx.tns` | A calculator. The binaries are built and structurally validated (§9); none of them has ever executed. |
| Booting Windows 95 | A legally obtained install image plus the above. |
| Confirming the rotated-panel direction | A CX of revision W or later. `WINSPIRE_PANEL_ROTATE_CCW` must be set empirically. |
| Confirming the dock UART MMIO base | Hardware. `WINSPIRE_CXLINK_UART_BASE` defaults to `0x90040000` by analogy with the classic Nspire; it is **not** confirmed for the CX SoC. |
| Audio end-to-end | Hardware. The mixer is now wired to the link and that path runs on the host, but no guest has ever programmed the Sound Blaster here, so *sound* has never been produced. |
| Network end-to-end | Hardware. The routing backend is implemented, builds clean, its DHCP server passes a host test, and provisioning runs end to end against a simulated bridge on the host; nothing has crossed a real link, so DHCP/NAPT/forwarding have never been *executed* against a real guest. |
| Wi-Fi association after provisioning | An ESP32 and an access point. The credential exchange is verified on the host; the association itself (`esp_wifi_connect`, the disconnect-reason mapping) has never run. |
| Running the ESP32 firmware at all | An ESP32 board. It builds, but nothing has ever been flashed to hardware. |
| Performance on hardware | A calculator, or at minimum a cycle-accurate ARM926 model. Host x86-64 numbers are **not** predictive of CX throughput — see PERFORMANCE.md. |

---

## Known gaps in the integration

These are places where the work is real but the wiring is unfinished, plus the
verification limits of the work that is wired. They are listed rather than
hidden; closed items are struck through so the history stays visible:

1. ~~The mixer is not yet feeding cxlink.~~ **Closed.** The mixer pull, the DMA
   refill, the S16→S8 conversion and the AUDIO_DATA framing are all implemented
   and run on the host; see §7. On the calculator they are type-checked only.
2. ~~`cxlink_poll()` is not yet called from the frontend.~~ **Closed.** It is
   called from `service_io_bridge()` in `main.c`, gated to once per millisecond
   of guest time.
3. ~~The ESP32 routing backend is a stub.~~ **Closed.** `net_forward_ipv4()` hands
   the frame to a real lwIP netif, NAPT and forwarding are configured and
   guarded by `#error`, and the DHCP server is written and tested. See
   [NETWORK_ARCHITECTURE.md](NETWORK_ARCHITECTURE.md) §6 for why the
   architecture is a NAT router rather than a raw 802.11 bridge.
4. **`WINSPIRE_CXLINK_UART` is off by default**, so a CX build links cxlink with
   a null transport. This is intentional: a silent no-op beats a hang on an
   unverified register address.
5. **No audio content has ever been produced.** The path is exercised, but only
   with a silent mixer. A test that programs the SB16 from the guest (DSP reset,
   sample rate, DMA transfer) would close this without hardware and does not
   exist yet.
6. **The audio service interval is a tuning choice, not a measurement.** The
   1 ms gate and the batch sizes in `cx_profiles.h` are derived from the guest
   clock arithmetic (documented in that header), not from ARM926 timings. They
   need revisiting once the port runs on a calculator.
7. **Nothing in the network path has run.** The strongest claim available is that
   the firmware builds clean, the DHCP server passes a host test, and the ACK
   endianness now matches the decoder. DHCP against a real Windows 95 guest, NAPT,
   forwarding and the Wi-Fi association are all unexecuted. The first hardware
   session should expect to fix things here rather than to watch it work.
8. ~~The uplink cannot be provisioned from the calculator yet.~~ **Closed.**
   `[network] ssid`/`password` in `winspire.ini` are validated by shared code on
   both ends and pushed over the link by `cxlink_net_provision()`, which
   retransmits until the bridge confirms and re-provisions it automatically after
   a bridge restart. See [NETWORK_ARCHITECTURE.md](NETWORK_ARCHITECTURE.md) §6.3.
   What is *not* closed is the association: no credential pair has ever been
   handed to a real radio, so the Wi-Fi half remains unexecuted (§6 above).
9. **There is no on-device way to type an SSID or password.** They come from
   `winspire.ini`, which means editing a file on the calculator or in the `.tns`
   build. This port has no text-input API in use, so a settings screen would be
   new work; the protocol does not care either way, since provisioning is just a
   `NET_CONFIG` frame.
10. **A link-state conflation was found and fixed while adding provisioning.**
   `cxlink_link_up()` used to be set from the bridge's *uplink* flag, so a bridge
   with no Wi-Fi credentials reported "link down" forever - which also muted the
   audio path and blocked the very DHCP exchange provisioning needs. Liveness is
   now derived from frames arriving (plus a keepalive and an 8 s timeout) and the
   uplink flag is reported separately; see NETWORK_ARCHITECTURE.md §6.4.

---

## What would have to be true for this to be "done"

In order, and each is a hard prerequisite for the next:

1. A CX unit, to observe the panel orientation and confirm the port boots the
   BIOS and reaches VGA output. (The `.tns` files exist - see §9 - this step
   is now purely observational.)
2. Confirmation of the dock UART base, or a scope on the dock Tx pin.
3. A Windows 95 image, to see a real desktop, and then a performance cycle on
   real ARM926 hardware.
4. For the audio and network half: an ESP32 board. Its firmware already builds
   and produces validated images (`make esp32`), but nothing has been flashed, so
   the I2S path, the UART link, the DHCP server against a real guest and the
   router have never executed.

Item 1 is the one that changes this from "carefully ported, built and type
checked" to "known to work".
