# AUDIO_ARCHITECTURE.md — Sound Blaster to physical speaker

## 1. Principle

Windows 95 must talk to **real PC sound hardware**, not to a Windows-specific
sound API. The Sound Blaster 16 is the right choice because Windows 95 ships a
driver for it, it uses ISA DMA and IRQs rather than a proprietary interface, and
it is what an application expecting "a PC with a sound card" will find.

The full path, with each hop in a real source file:

```
application (Windows 95)
    |  Win95 SB16 driver
    v
x86 I/O ports 0x220..0x22F, 0x388..0x38B + IRQ 5 + 8-bit DMA channel 1
    v
source/winspire/sb16.c            DSP, mixer, DMA transfer, IRQ
  + source/winspire/adlib.c       OPL2 FM (0x388)
  + source/winspire/pcspk.c       PC speaker
    v
pc_audio_step()   (source/winspire/pc.c)   mixer_callback() -> 16-bit stereo,
    |                                      then i8257_dma_run() so the card refills
    v
cxlink_audio_resampler_feed()     (source/winspire/cxlink.c)
    |  s16 stereo 44100 -> mono s8 at the negotiated rate, held to a full block
    v
cxlink_audio_write()              (source/winspire/cxlink.c)
    |  framed as AUDIO_DATA
    v
cxlink framing + CRC + sequence   (cxlink.h)
    v
SoC UART on the dock connector (115200 8N1, TTL)
    v
ESP32 UART1 -> AUDIO_DATA frames  (source/esp32/main/main.c)
    v
ESP32 audio ring buffer (8 KiB, 32 blocks)
    v
I2S -> DAC -> speaker
```

Nothing in that chain is Windows-specific, and nothing in it is bypassed when
the ESP32 is absent — the mixer still runs; only the transport has no consumer.

---

## 2. The link budget is the design constraint

115200 8N1 is **11 520 bytes/s exactly**. Framing costs 10 bytes per frame
(8-byte header + 2-byte CRC), so:

| Configuration | Payload/s | Link use |
|---|---|---|
| 8 kHz mono, 8-bit, 128-sample blocks | 8 000 B/s | ~8 620 B/s (75%) |
| 8 kHz mono, 16-bit | 16 000 B/s | **does not fit** |
| 11 kHz mono, 8-bit | 11 000 B/s | **does not fit** |
| 5.5 kHz mono, 8-bit | 5 500 B/s | ~5 930 B/s (51%) |
| 8 kHz stereo, 8-bit | 16 000 B/s | **does not fit** |

So **the defaults are 8 kHz, mono, 8-bit signed** — the best quality that leaves
headroom for the network channel. That is a deliberate, documented trade, not an
oversight, and it is why `AUDIO_CONFIG` exists: the calculator negotiates the
rate and format at runtime so a user can lower the rate to buy network
throughput, or raise it (16-bit) when networking is unused.

`CxlinkAudioConfig` carries `sample_rate`, `channels`, `format` (8-bit signed or
16-bit signed LE), a `mute` flag, and `volume` (0..256, 256 = unity).

---

## 3. The rule that governs everything: the CPU never blocks

> "The x86 CPU emulator must NEVER block waiting for the ESP32 audio device. If
> the ESP32 temporarily stops responding, queue/recover rather than freezing
> Windows 95."

This is honoured structurally rather than by convention:

- **The guest side has no blocking call anywhere in the path.** The frontend
  calls `pc_audio_step()` on a timer and `cxlink_audio_write()` only appends to a
  ring buffer. There is no wait, no retry, no spin. In particular the pull is
  driven by *elapsed time*, never by how full the ESP32's buffer is.
- **The pull happens even when the link is down.** `pc_audio_step()` is
  unconditional, because the native build's `pc_step()` no longer advances ISA
  DMA and a stalled refill would wedge the guest's audio driver rather than just
  going quiet.
- **Audio is the unreliable channel.** `AUDIO_DATA` frames carry no
  `CXLINK_FLAG_ACK_REQ`. Nothing waits for acknowledgement, because nothing could
  usefully act on one: a sample that is 40 ms late is not a sample, it is noise.
- **Loss is drop-newest at the ring, drop-oldest at the receiver.**
  `cxlink_send()` discards the block when the TX ring is full (a partial frame is
  worse than no frame, so it rewinds the ring). The ESP32 counts a failed
  `xRingbufferSend` as an underrun. Either way the guest is unaffected.
- **The transport is non-blocking by contract.** `CxlinkHal::write` returns the
  number of bytes accepted and is allowed to accept fewer than asked;
  `cxlink_flush()` pushes the unsent tail back into the ring and returns.
- **The ESP32 side never blocks the link task either.** `audio_pump()` uses a
  zero-tick `xRingbufferReceive` and `i2s_channel_write(..., 0)`, so a slow DAC
  cannot back-pressure the UART, which would in turn stall the calculator.

The failure mode when the ESP32 is unplugged is therefore **silence plus a rising
underrun counter**, not a hang. That is the correct behaviour.

---

## 4. Recovery and observability

| Situation | Behaviour |
|---|---|
| TX ring full | Audio block dropped, `audio_dropped` incremented |
| ESP32 not answering | `AUDIO_DATA` keeps being sent; audio simply does not come out |
| ESP32 resets mid-stream | It sends `VERSION` on boot; the calculator's outstanding *network* frame is cleared on `RESET`/timeout, audio needs no recovery |
| ESP32 buffer full | `xRingbufferSend` fails, counted as an underrun |
| Host sends `AUDIO_FLUSH` | ESP32 drains its ring so playback restarts cleanly |
| Guest changes rate/format | `AUDIO_CONFIG` reconfigures I2S; the old channel is torn down first |

`cxlink_get_status()` exposes `underruns`, `overruns` (dropped blocks) and
`link_errors`, and the ESP32 returns the same in `AUDIO_STATUS`. These are the
numbers to watch when tuning: a rising `overruns` means the link is saturated and
the rate must come down, `underruns` means the ESP32 cannot keep the DAC fed.

The ESP32 replies to `AUDIO_CONFIG` with `AUDIO_STATUS` whose `queued_frames`
field is reported as the **free space left in the audio ring in blocks**, so the
calculator can watch the buffer draining *before* it becomes an underrun.

---

## 5. From mixer block to wire block

Three layers, and the boundaries between them are load-bearing:

```
pc_audio_step()                pc.c      pull one mixer block, advance ISA DMA
cxlink_audio_resampler_feed()  cxlink.c  s16 stereo 44100 -> mono s8, buffered
cxlink_audio_write()           cxlink.c  frame whatever it is given as AUDIO_DATA
```

`pc_audio_step()` is the reason audio works at all on the calculator. The native
build compiles the ISA DMA step out of `pc_step()` for speed (see
`TINY386_SPEED_BUILD` in `pc.c`) and nothing else calls `i8257_dma_run()`. Without
this pull the Sound Blaster's buffer can never refill, and a guest driver waits
forever for the interrupt a completed DMA block is supposed to raise. So it runs
**unconditionally**, even with no ESP32 attached; only the conversion and framing
are skipped when the link is down.

The resampler is nearest-sample, and deliberately so: a polyphase filter would be
better audio and is explicitly **not** used. On a 130–160 MHz ARM926 it would cost
more cycles than the audio is worth, and the link — not the resampler — is the
quality limit at 8 kHz mono 8-bit. A future reader should not "fix" this without
also raising the link rate.

One decision there was forced by measurement rather than taste. The frontend
services the bridge about once a millisecond, which at 8000 Hz produces **8
samples**. Framing that on its own spends 10 header bytes on 8 bytes of audio:
18 KB/s against an 11.5 KB/s link, which does not fit. So the resampler holds a
partial block and emits only full `CXLINK_AUDIO_BLOCK` (128-sample) frames, which
brings the cost back to ~1.08 bytes per sample at any tick rate. The rule is
**never frame a partial block just because the service tick ended.**

The other half of that decision is the fractional sample position. A 44100 → 8000
conversion emits 0.1814 samples per input frame, so truncating it per call would
run audio slow and audibly wrong. It carries in the resampler struct.

---

## 6. Status and remaining work

| Item | State |
|---|---|
| `cxlink.h` protocol, formats, budget | written, shared with the ESP32, type-checked |
| `cxlink.c` framing, CRC, ring, resampler, drop policy | **typechecked** + **executable self test** (`make selftest`) |
| `sb16.c` / `adlib.c` / `pcspk.c` / mixer | pre-existing, unchanged |
| `pc_audio_step()` — mixer pull plus DMA refill | **verified running** on the host, type-checked for the CX |
| Frontend call site (`service_io_bridge()` in `ndless/main.c`) | wired; **typechecked** only for the CX |
| ESP32 I2S path, ring buffer, underrun accounting | **builds clean** (ESP-IDF v5.5.5, xtensa-esp-elf 14.2.0); never run against a real DAC |
| Audio with real content (guest actually programs the SB16) | not attempted — every run here has a silent mixer |
| End-to-end audio on hardware | not attempted |

What the self test covers, byte-exactly: s16 extremes surviving the `>>9` with no
clipping (127 and -128), the partial-block hold, the fractional carry, and the
44100 → 8000 block arithmetic. What it does not cover: the ESP32 side, and
whether a real Sound Blaster playback ever produces non-silence.
