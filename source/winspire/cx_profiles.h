/*
 * TI-Nspire CX build profiles.
 *
 * Select one with -DWINSPIRE_PROFILE_DEBUG, -DWINSPIRE_PROFILE_RELEASE or
 * -DWINSPIRE_PROFILE_TURBO. build_cx.sh sets this from its profile argument.
 *
 * These knobs deliberately do NOT change which hardware is emulated. TURBO is
 * still a complete PC: full x86 (real mode, protected mode, paging), BIOS, VGA,
 * IDE, PS/2 keyboard and mouse, PIC, PIT, DMA, CMOS/RTC, Sound Blaster and the
 * Ethernet adapter. TURBO only changes how much *diagnostic and presentation*
 * work happens per emulated instruction, plus the batch sizes that decide how
 * often the guest is interrupted for input, video and timers.
 *
 * Anything that would remove emulated hardware belongs in a different product,
 * not in this header.
 */
#ifndef WINSPIRE_CX_PROFILES_H
#define WINSPIRE_CX_PROFILES_H

#if !defined(WINSPIRE_NATIVE_BUILD)
#error cx_profiles.h is only meaningful for the native (calculator) target
#endif

/*
 * Written out rather than factored into a macro: `defined` inside a macro
 * expansion is not portable and GCC warns about it under -Wextra
 * (-Wexpansion-to-defined).
 */
#if ((defined(WINSPIRE_PROFILE_DEBUG) ? 1 : 0) + \
     (defined(WINSPIRE_PROFILE_RELEASE) ? 1 : 0) + \
     (defined(WINSPIRE_PROFILE_TURBO) ? 1 : 0)) != 1
#error Select exactly one of WINSPIRE_PROFILE_DEBUG/RELEASE/TURBO
#endif

/* ------------------------------------------------------------------ */
/* DEBUG: maximum diagnostics and hardware tracing.                    */
/* ------------------------------------------------------------------ */
#ifdef WINSPIRE_PROFILE_DEBUG

#undef TINY386_NO_LOG
#undef TINY386_SPEED_BUILD
#define I386_DIAG_TRACE 1
#define WINSPIRE_TRACE_IO 1
#define WINSPIRE_TRACE_EXCEPTIONS 1
#define WINSPIRE_VGA_FULL_REDRAW 1
/* One instruction per batch so traces interleave with guest time. */
#define TINY386_PC_STEP_COUNT 1
#define TINY386_INPUT_POLL_LOOPS 1U
#define TINY386_VIDEO_POLL_LOOPS 1U
#define WINSPIRE_PROFILE_NAME "DEBUG"

/* ------------------------------------------------------------------ */
/* RELEASE: normal optimized build.                                    */
/* ------------------------------------------------------------------ */
#elif defined(WINSPIRE_PROFILE_RELEASE)

#define WINSPIRE_TRACE_IO 0
#define WINSPIRE_TRACE_EXCEPTIONS 0
#define WINSPIRE_VGA_FULL_REDRAW 0
/*
 * Batch size sets the guest-time quantum for everything the frontend services
 * outside the interpreter: input, video, the PIT rebase and the ESP32 I/O
 * bridge. One batch is
 *
 *     PC_STEP_COUNT * INS_CYCLES / clock_hz   seconds of guest time
 *     4096 * 12 / 4770000                    = 10.3 ms  (RELEASE default)
 *
 * 10 ms keeps the bridge service and the video blit inside one ~100 Hz tick,
 * which is the finest granularity worth paying per-batch overhead for on this
 * core. Lowering clock_hz lengthens the quantum proportionally.
 */
#define TINY386_PC_STEP_COUNT 4096
#define TINY386_INPUT_POLL_LOOPS 2U
#define TINY386_VIDEO_POLL_LOOPS 1U
#define WINSPIRE_PROFILE_NAME "RELEASE"

/* ------------------------------------------------------------------ */
/* TURBO: maximum practical performance, full PC emulation retained.   */
/* ------------------------------------------------------------------ */
#elif defined(WINSPIRE_PROFILE_TURBO)

#define WINSPIRE_TRACE_IO 0
#define WINSPIRE_TRACE_EXCEPTIONS 0
#define WINSPIRE_VGA_FULL_REDRAW 0
/*
 * Twice the RELEASE batch, 8192 * 12 / 4770000 = 20.6 ms of guest time.
 *
 * A batch amortizes the per-batch device/timer service cost over more guest
 * instructions, but it also caps the frame rate (video is serviced once per
 * batch, so 16384 would ceiling the panel at 24 fps no matter how fast the
 * core runs) and it sets how much PCM the I/O bridge must move per call
 * (41 ms of 44.1 kHz audio is two full mixer blocks). 20 ms is the point where
 * those costs are still small against the interpreter and the panel can reach
 * ~48 fps. It is kept well below 65536 because Windows 95's BIOS and drivers
 * poll the PIT and the VGA retrace; starving those loops shows up as a stall,
 * not as slowness.
 */
#define TINY386_PC_STEP_COUNT 8192
/* Halve the input sampling interval: the clickpad is cheap to sample and the
 * cursor feels laggy at the RELEASE interval. */
#define TINY386_INPUT_POLL_LOOPS 1U
#define TINY386_VIDEO_POLL_LOOPS 1U
/* Interpreter fast paths that are worth their code size on ARM926. */
#ifndef TINY386_ARM_FAST
#define TINY386_ARM_FAST 1
#endif
#ifndef REP_SLICE_ENABLED
#define REP_SLICE_ENABLED 1
#endif
#ifndef REP_SLICE
#define REP_SLICE 8192
#endif
#ifndef BULK_REP_ENABLED
#define BULK_REP_ENABLED 1
#endif
#define WINSPIRE_PROFILE_NAME "TURBO"

#endif

#endif /* WINSPIRE_CX_PROFILES_H */
