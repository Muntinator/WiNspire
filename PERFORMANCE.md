# PERFORMANCE.md — measurement, methodology, and results

## 0. Read this first

**The numbers in this document are from an x86-64 development host, not from a
TI-Nspire CX.** They are useful for three things and useless for one:

- **Useful:** they are a *repeatable, deterministic* measure of the emulator
  core, so a change that makes the core slower shows up immediately and
  objectively. That is what a benchmark is for, and it is what made the
  optimisation work in this port safe to attempt.
- **Useful:** the per-phase breakdown locates *which instruction classes* are
  expensive, independently of host ISA.
- **Useful:** they establish a baseline for someone with hardware to beat.
- **Useless:** as a prediction of CX throughput. An ARM926EJ-S at ~130–160 MHz
  with 16 KiB I-cache / 8 KiB D-cache and no integer divide instruction has
  almost nothing in common with a modern superscalar x86-64. Expect the CX to be
  well over an order of magnitude slower. Do not scale these numbers.

**No CX-hardware benchmark numbers exist yet.** Anyone claiming otherwise for
this repository would be making them up.

---

## 1. How to reproduce everything here

```sh
# 1. Build the headless host frontend (same core config as the calculator).
source/build-scripts/build_host.sh

# 2. Assemble the benchmark payload into a bootable floppy image.
source/build-scripts/build_bench.sh

# 3. Run it: the emulated BIOS boots the payload, which runs eight phases.
build/Host/winspire-host --boot build/bench/bench386.img --seconds 60

# Per-phase transitions with guest timestamps:
build/Host/winspire-host --boot build/bench/bench386.img --seconds 60 --trace-phases

# Regression check that no media is needed for:
build/Host/winspire-host --bench 200

# Type-check the CX target without the Ndless SDK:
source/build-scripts/check_cx_frontend.sh          # TURBO
source/build-scripts/check_cx_frontend.sh . DEBUG
source/build-scripts/check_cx_frontend.sh . RELEASE

# Bridge/protocol checks; no calculator and no ESP32 needed:
build/Host/winspire-host --selftest
```

On this host those take about 3 s for the boot run, 1 s for `--selftest` and 45 s
for the three type-checks.

---

## 2. Benchmark design

### 2.1 Why a custom payload

The brief asks for repeatable benchmarks and warns against optimising without
measuring. Booting Windows 95 is neither repeatable nor a measurement — it is
minutes of I/O-bound work with a large variance. So the CPU benchmark is a
purpose-built real-mode boot sector, `source/bench/bench386.S`, with a **fixed**
instruction mix. It is booted by the emulated BIOS, so it still exercises the
real pipeline (POST, real-mode decode, memory translation, device I/O), and then
runs eight phases:

| Phase | Instruction mix | Loop count |
|---|---|---|
| 0 | 32-bit ALU chain: add/xor/sub/or/and/cmp/adc | 400 000 |
| 1 | register moves incl. `movzx`/`movsx` | 400 000 |
| 2 | 32-bit memory load/store | 200 000 |
| 3 | `push`/`pop`/`call`/`ret` | 150 000 |
| 4 | `rep stosl` / `rep movsl` / `rep stosb` | 4 000 outer |
| 5 | shifts, rotates, `imul`, `mul` | 200 000 |
| 6 | conditional branches, taken and not taken | 300 000 |
| 7 | 8-bit and 16-bit operations | 200 000 |

Each phase publishes its number to a word in guest RAM, which the harness samples
to attribute time and step counts per phase — no emulator-side hooks required.

### 2.2 Design notes that mattered

Three bugs in the payload itself had to be fixed before the numbers meant
anything. They are recorded because they are the kind of thing that silently
produces plausible garbage:

1. **A 16-bit real-mode store cannot address `0x50000`.** `movw $7, 0x50000`
   silently truncated the *address* to zero, so phase markers were being written
   over the interrupt vector table. Fixed by parking `ES` at `0x5000` and
   storing through a segment.
2. **Real mode defaults to 16-bit address size.** `rep stosl` with `EDI =
   0x60000` and no prefix uses `DI`, i.e. offset zero — writing 8 KiB blocks
   over low memory on every iteration. Every memory operand above 64 KiB now
   carries an explicit `addr32` prefix.
3. **Zeroed RAM reads as `0`, which is a valid phase number.** The harness
   therefore mistook BIOS POST for phase 0, which both inflated the "phase 0"
   time and made POST look like emulator slowness. Fixed by pre-loading the phase
   word with a sentinel from the harness.

### 2.3 What "steps" means

The counter is emulator dispatch steps (`cpui386_get_cycle`), which is what the
interpreter actually does per iteration. **`REP`-prefixed opcodes retire in
bulk**, so phase 4 shows few steps for a lot of real work: it moves ~73 MiB in
46 ms (~1.6 GiB/s) while reporting only 65 536 steps. Phase 4's per-step figure
is therefore *not* comparable with the others, and the two headline numbers in
the report are separated for that reason.

---

## 3. Results (host x86-64, `-O3 -march=native`)

### 3.1 Steady state

```
ph  instruction mix                                 steps        sec   Msteps/s
0   alu32 add/xor/sub/or/and/cmp/adc              5111971     0.0452     112.99
1   register mov / movzx / movsx                  3997696     0.0346     115.39
2   32-bit memory load/store                      2621444     0.0367      71.47
3   push/pop/call/ret                             1769472     0.0227      77.79
4   REP movs/stos (16K per iter)                    65536     0.0468       1.40   (see 2.3)
5   shift / rotate / mul / imul                   2228232     0.0234      95.27
6   branches (jcc taken+not taken)                2555904     0.0232     110.40
7   8-bit and 16-bit ops                          2147421     0.0179     119.65
payload steady-state: 20497676 steps / 0.251 s = 81.80 Msteps/s
```

Repeat-run spread across five runs of the same binary: **76–82 Msteps/s**
steady state, i.e. about ±4%. Differences smaller than that are noise.

A later session on the same host measured this figure between 91 and
107 Msteps/s (§3.4), i.e. the spread between *sessions* is wider than the spread
within one. Treat any payload comparison as needing several runs on the same
machine, and prefer the before/after pair in §3.4 over an absolute number.

Other figures from the same run:

```
bios post         : 3860241 instructions in 2.606 s
vga step          :  83.2% of wall  1735007 calls   1.4 us/call
cpu step          :  13.7% of wall  1746224 calls   0.2 us/call
audio step        :   0.1% of wall     2747 calls   1.5 us/call  126155 mixer frames
io bridge         : link up, 22784 link samples, 24624 frame bytes to transport
```

### 3.1.1 The audio and bridge path costs almost nothing

The `audio step` and `io bridge` lines are new: the loop now services the shared
mixer and the cxlink bridge the same way the calculator frontend does (see
[AUDIO_ARCHITECTURE.md](AUDIO_ARCHITECTURE.md)). Three things are worth reading
out of them.

**The rate arithmetic checks out against the wall clock.** 126 155 mixer frames
over a 2.86 s run is 44 110 frames/s, against a mixer rate of 44 100 Hz — 0.02%
high. That is the accumulated-fraction pacing being correct rather than drifting,
which matters because the same accumulator sets audio pitch.

**The cost is negligible, and measurable.** 2747 calls at 1.5 us/call is 0.1% of
wall time, and it is *amortised*: the pull is gated to once per millisecond of
elapsed time, so the count tracks the interval rather than the loop. The bad
version of this design would pull the mixer every `pc_step()` — 1.6 M calls here
instead of 2747 — which is why the gate exists and why `pc_audio_step()` is not
called from inside the interpreter.

**The framing overhead is 1.078 bytes per sample, and it was 2.25 before.**
24 624 bytes for 22 784 samples is 178 full 128-sample frames at 138 bytes each,
plus 60 bytes of handshake and keepalive. The first working version of this
wiring framed whatever a 1 ms service tick produced — about 8 samples — which
spent 10 header bytes per 8 bytes of audio: roughly 18 KB/s against an 11.5 KB/s
link. It would have overflowed the TX ring and dropped audio on a busier guest.
The fix was to hold a partial block in the resampler and only emit full
`CXLINK_AUDIO_BLOCK` frames, which also makes the cost independent of the tick
rate. At 8000 Hz that is 8.6 KB/s of audio, leaving ~2.9 KB/s for Ethernet —
matching the budget in `cxlink.h`.

One limit of this measurement: the guest never programs the Sound Blaster during
a BIOS boot, so the mixer output is silence (`0 non-silent` blocks). These numbers
measure the *machinery* — DMA refill, resampling, framing, ring — not sound.

### 3.2 What the profile says

- **Memory access is the slowest translation-heavy class** (phase 2, ~72
  Msteps/s vs ~115 for register-only work). That is the expected shape: address
  translation plus the TLB path on every load and store.
- **8-bit and 16-bit operations are the fastest** (phase 7, ~115–120
  Msteps/s), followed by register moves and branches. Plain ALU dispatch is not
  the bottleneck.
- **BIOS POST is dominated by waiting, not by execution.** 3.86 M steps in
  2.6 s is ~1.5 Msteps/s, and `vga step` accounted for 74–83% of wall time —
  the guest is halted waiting on the emulated clock while the VGA device keeps
  being serviced. §3.4 explains what that turned out to cost and what was done
  about it. This is why the headline "instructions/sec" over a whole run is
  misleading and why the report prints the payload steady-state figure
  separately. It is also a real observation about the CX: **POST time is set by
  timing/halt behaviour, not by raw interpreter speed**, so it is not something
  the CPU interpreter can fix.
- **`PC_STEP_COUNT` sets the sampling granularity, not the throughput.** The
  harness attributes each phase boundary to a batch, so very short phases are
  quantised. Phases here are large enough that this is a sub-1% effect, except
  for phase 4 (single batch).

### 3.4 The measured cost of redrawing nothing (optimised)

`vga step` was the largest single term in every profile taken before this
change: 74–83% of wall time, against 14% for the interpreter. That is a
surprising thing for a display device to cost, so it was measured rather than
assumed.

**What the guest was actually doing.** The benchmark payload never leaves text
mode, so every refresh went through `vga_text_refresh()`, which walks all
`width * height` character cells — 80 x 25 = 2000 — and compares each one against
a cached copy to find the ones that changed. A counter placed in that loop
settled it:

```
probe: refresh=600000 mode=1 text_tr=599999 cells=1199998000 dirty=604088
```

600 000 refreshes examined **1 200 million** cells and found **604 thousand**
dirty. 99.95% of the work was spent confirming that a screen nobody was looking
at had not changed. The dirty count was also revealing in itself: exactly one
cell per refresh, because the cursor cell was excluded from the cache test and
so was re-blitted on every single poll (133 ms is the blink period, but the
underlined cell was being redrawn ~300 000 times a second, not 7).

**Why the refresh rate was what it was.** `vga_step()` advances the retrace
phase per poll and returns "redraw" on every third poll, so the refresh rate is
`3 / poll interval` and not a chosen frame rate at all. On a halted guest a poll
costs almost nothing, so the poll rate goes to the CPU's limit and the refresh
rate goes with it. That is the same shape as the POST profile in §3.2: the
device was being serviced as fast as the machine could service it, because
nothing in the design said not to be.

**What changed** (`source/winspire/vga.c`):

1. `raster_content_gen` is a counter bumped by every guest VRAM store
   (`vga_mem_write`, `_write16`, `_write32`, `_write_string`), every VGA and VBE
   register write, `vga_set_force_8dm()`, and the mode/font reset. If it has not
   moved, no pixel on screen can differ. A refresh that sees it unchanged returns
   before rebuilding the attribute palette and before touching a character cell.
2. The cursor cell stopped being excluded from the cache test. It is
   invalidated explicitly when the blink phase toggles and when the cursor moves
   (it already was on a move), so the underline still appears and disappears, at
   the 133 ms rate it is supposed to have.

**Measured, three runs of the same binary each way, same host:**

| | before | after |
|---|---|---|
| `vga redraw regions` | 842 220 | **25** |
| `vga step`, share of wall | 74.3% | **16.6%** |
| `cpu step`, share of wall | 14.6% | 36.0% |
| `pc_step` calls per run | 2 526 659 | 10 999 035 |
| `instructions/sec` (headline) | 8.69 MIPS | 8.70 MIPS |
| payload steady-state | 102.6 Msteps/s | 104.5 Msteps/s |

**Reading this table honestly.** The two throughput figures at the bottom did not
move, and they were not expected to: the payload issues one poll per 65 536
instructions, so the renderer is ~0.001% of that phase, and the headline
`instructions/sec` is pinned by a fixed 2.6 s POST timer wait (§3.2). The change
is not visible in either, and any claim that it "raised the frame rate" would be
unsupported. What it did is cut the VGA device path from three quarters of the
profile to a sixth, and in exchange the loop now completes **4.35x more poll
cycles per second** (10 999 035 / 2.8 s against 2 526 659 / 2.8 s) during exactly
the phase where the guest is halted. Those cycles are the ones that re-arm the
PIT, deliver the timer interrupt and service the bridge, so the win is lower
timer-interrupt latency and more headroom for the device layer, not a faster
interpreter.

Of the 16.6% that remains, about 3.5 points are the host's own
`clock_gettime` — measured by substituting a cached value for `get_uticks()` in
the refresh path, which dropped `vga step` to 13.2% — and most of the remainder
is the harness's own timing overhead, because `vga_ns` is measured as
`host_ns() - t0` and therefore includes the cost of the `host_ns()` call that
closes the interval. **On the CX this residual does not exist in the same form:
`get_uticks()` there returns a plain `guest_ticks` global rather than reading the
clock.** So the honest claim for the target is that the text-refresh path is now
close to free per poll, not that it is 16.6% of anything.

**Correctness.** The rendered output must not change, only how often it is
produced. `--screen` was added to the host harness for exactly this: it dumps the
character cells straight out of emulated VRAM. Captured at the end of a
`--boot build/bench/bench386.img` run, the full 80x25 SeaBIOS POST screen
(version banner, "Press ESC for boot menu.", "Booting from Floppy...") and the
`snapshot signature: 0x177104f5` are **byte-identical before and after**. All
three CX profiles still build warning-free under the cross toolchain and
`check_cx_frontend.sh` is clean.

### 3.5 A non-finding worth recording

`lpgno % tlb_size` appears in the TLB lookup path. On ARM926EJ-S an integer
modulo by a *variable* is a library call costing tens of cycles, so this would
have been the single best optimisation target. It does not apply: `tlb_size` is a
compile-time constant (`#define tlb_size 256/512/1024`, always a power of two),
so the compiler emits an AND. The new DTLB uses the same discipline — `lpgno &
(NSPIRE_DTLB_FAST_SLOTS - 1)` — and `cxlink`'s byte rings are power-of-two sizes
with the same reasoning documented in place.

---

## 4. Optimisation status

Honest summary: **the measurement infrastructure was the deliverable of the
first pass, and §3.4 is the first optimisation it paid for.**

The brief says "always optimise measured bottlenecks". The prerequisite for that
is having a bottleneck measurement that is trustworthy, deterministic, and
resistant to the three silent-garbage bugs listed in §2.2. That exists, and it
is what anyone continuing this work should use.

What has been optimised, on measurement:

- **Text-mode refresh no longer rescans a static screen** (§3.4). This was the
  single largest term in the profile at 74% of wall time, and it was pure waste:
  99.98% of the character cells it examined were provably unchanged. It is now a
  single counter comparison.

What is deliberately *still not* done, and why:

- **No speculative interpreter rewrites.** Without profiling on ARM926 (no
  `perf`, no hardware), changing the dispatch structure would be optimising for
  the wrong CPU. The host's relative costs are not the ARM926's: the ARM926 has
  no branch predictor to speak of, a 16 KiB I-cache, limited load/store units,
  and no divide. A change tuned on x86-64 could easily be a regression on the
  CX.
- **No retuning of `TINY386_PC_STEP_COUNT`.** The text-renderer fix removed most
  of the per-poll cost that a smaller batch was paying for, which makes a larger
  batch more attractive — but the batch size is currently chosen from
  guest-clock arithmetic, and the crossover is a property of ARM926 timings that
  only hardware can report. Retuning it blind would be the same mistake as a
  speculative interpreter rewrite, just at a different level.
- **No lazy-flag or register-caching changes.** These touch x86 *correctness*
  (`EFLAGS`, segment state, paging). The brief is explicit that correctness is
  not negotiable for benchmark numbers, and there is no way to validate them
  against Windows 95 here.
- **What is in place and safe:** the TURBO profile in
  `source/winspire/cx_profiles.h` (larger instruction batches to amortise
  per-batch device service, faster input sampling, interpreter fast paths
  enabled), the CX's supported `set_cpu_speed(CPU_SPEED_150MHZ)` overclock path
  with restore-on-exit, hot/cold section separation in `build_cx.sh`, and
  `-ffunction-sections`/`-fdata-sections`/`--gc-sections` so diagnostics and
  unused backends are dropped from the image.

---

## 5. Next measurements to take, in order

1. **On hardware:** steps/sec during Windows 95 boot and desktop idle. The
   harness prints exactly these metrics; only the platform changes.
2. **On hardware:** confirm whether the CX's `set_cpu_speed(CPU_SPEED_150MHZ)`
   actually takes, and how much throughput it buys. That is the cheapest known
   win and it is measurable in one run.
3. **On hardware:** audio underrun rate against the 11 520 B/s link budget
   (`cxlink_get_status()` exposes the counters), and what `audio step` costs on
   an ARM926. 1.5 us/call on x86-64 says nothing about the CX; the useful number
   is its share of a 10 ms batch.
4. **A guest that actually plays sound.** Programming the SB16 from the
   benchmark payload (DSP reset, sample rate, DMA transfer) would turn the audio
   measurements above from "the machinery runs" into "the machinery moves real
   PCM", with no hardware needed. This is the highest-value missing test.
5. **On hardware:** re-tune `TINY386_PC_STEP_COUNT` (now 4096 RELEASE / 8192
   TURBO, down from 16384 for TURBO). The value is chosen from guest-clock
   arithmetic in `cx_profiles.h`, not from ARM926 timings: 8192 instructions is
   8192 x 12 / 4.77 MHz = 20.6 ms of guest time, which is the interval at which
   input, video, the PIT and the bridge are all serviced, and therefore the
   ceiling on frame rate (48 fps) and the audio service granularity. Smaller
   batches mean more per-batch overhead; only a calculator can say where the
   crossover is.
6. **With an ARM toolchain and an emulator:** `-Os` vs `-O3` per source file for
   the hot set in `build_cx.sh`. That is a cheap, low-risk sweep that the
   current script makes trivial to run.
7. **Only then:** interpreter-level work, armed with a real profile.

---

## 6. Reporting template

For any future change, record:

```
commit:                <sha>
profile:               DEBUG | RELEASE | TURBO
steady-state:          XX.XX Msteps/s      (baseline 81.80)
per-phase deltas:      phase 2  ±X%, phase 6 ±X%, ...
correctness check:     payload reaches phase 0xFF; check_cx_frontend.sh clean
notes:                 anything measured under unusual load
```

A change that does not move steady-state outside the ±4% noise floor should not
be reported as an improvement.
