/*
 * WiNspire headless host frontend.
 *
 * Development/measurement frontend for the shared emulator core in
 * source/winspire/. It builds the *same* core sources with the *same*
 * WINSPIRE_NATIVE_BUILD + release_config.h configuration as the calculator
 * build, but renders into a plain host framebuffer and reports throughput
 * instead of touching calculator hardware.
 *
 * It exists so the x86 core can be built, profiled and benchmarked on a
 * workstation while the repository stays buildable without the Ndless SDK.
 *
 * Modes:
 *   --boot <img>     attach <img> as floppy A: (see source/bench/bench386.S)
 *   --bench <M>      run M million guest instructions (no media needed)
 *   --seconds <S>    wall-clock cap, default 120
 *   --ini <path>     load a PCConfig INI (bios/vga_bios/mem_size/hda/...)
 *   --quiet          suppress the phase table
 *   --trace-phases   log each benchmark phase transition
 *   --selftest       run the cxlink bridge/protocol self test and exit
 *
 * With --boot the run ends when the benchmark payload writes 0xFF to the
 * phase word at physical address 0x8000, and a per-phase MIPS table is printed.
 *
 * The loop also drives the PC audio path through pc_audio_step(), with the same
 * guest-time pacing the calculator frontend uses, so the Sound Blaster DMA
 * refill and mixer pull are exercised and timed on real runs.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#include "pc.h"
#include "ini.h"
#include "cxlink.h"
#include "bridge_net.h"

#ifndef BPP
#define BPP 16
#endif

#define DEFAULT_WIDTH 320
#define DEFAULT_HEIGHT 240
#define DEFAULT_WALL_SECONDS 120
#define PHASE_WORD_ADDR 0x50000
#define PHASE_DONE 0xFF
#define MAX_PHASES 32

/*
 * Crash forensics. A host signal (SIGFPE from an emulated divide, SIGSEGV
 * from a bad guest access) otherwise kills the process before the buffered
 * report is flushed, which makes a failing payload look like an empty run.
 * These globals are updated once per emulation batch so the handler can say
 * which phase was executing.
 */
static volatile unsigned g_phase = 0xFFFFFFFFu;
static volatile unsigned long long g_instructions;
static volatile int g_in_pc_step;

static void crash_report(int sig)
{
	char buffer[192];
	int length = 0;
	const char *name = sig == SIGFPE ? "SIGFPE" :
		sig == SIGSEGV ? "SIGSEGV" : "signal";
	const char *where = g_in_pc_step ? "inside pc_step" : "in host code after pc_step";

	length += snprintf(buffer + length, sizeof(buffer) - (size_t)length,
			   "\nhost: FATAL %s at phase %u, %llu instructions retired (%s)\n",
			   name, g_phase, g_instructions, where);
	if (length > 0)
		(void)write(2, buffer, (size_t)length);
	_exit(4);
}

static void install_crash_report(void)
{
	signal(SIGFPE, crash_report);
	signal(SIGSEGV, crash_report);
	signal(SIGBUS, crash_report);
	g_in_pc_step = 0;
}

static const char *phase_names[MAX_PHASES] = {
	"alu32 add/xor/sub/or/and/cmp/adc",
	"register mov / movzx / movsx",
	"32-bit memory load/store",
	"push/pop/call/ret",
	"REP movs/stos (16K per iter)",
	"shift / rotate / mul / imul",
	"branches (jcc taken+not taken)",
	"8-bit and 16-bit ops",
};

/* ------------------------------------------------------------------ */
/* Platform HAL required by the shared core (pc.h).                     */
/* ------------------------------------------------------------------ */

static uint64_t host_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t boot_ns;

/*
 * Guest time. The calculator frontends derive this from executed guest cycles
 * so host stalls cannot shift guest timer behaviour. The host harness uses a
 * monotonic host clock, which is what the headless profile wants and keeps
 * BIOS polling loops from spinning against a frozen emulated clock.
 */
uint32_t get_uticks(void)
{
	return (uint32_t)((host_ns() - boot_ns) / 1000ULL);
}

/*
 * The calculator frontend pre-reserves the guest and VGA blocks so the Ndless
 * heap is not fragmented at boot. The host has no such constraint.
 */
void *bigmalloc(size_t size)
{
	void *memory = calloc(1, size);

	if (!memory)
		abort();
	return memory;
}

void *pcmalloc(long size)
{
	void *memory = calloc(1, (size_t)size);

	if (!memory)
		abort();
	return memory;
}

int load_rom(void *guest_memory, const char *path, uword address, int backward)
{
	FILE *file;
	long file_size;
	size_t bytes_read;

	if (!path || !path[0])
		return 0;
	file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "host: cannot open ROM '%s'\n", path);
		abort();
	}
	if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0) {
		fclose(file);
		fprintf(stderr, "host: cannot size ROM '%s'\n", path);
		abort();
	}
	rewind(file);
	if (backward)
		bytes_read = fread((uint8_t *)guest_memory + address - file_size,
				   1, (size_t)file_size, file);
	else
		bytes_read = fread((uint8_t *)guest_memory + address, 1,
				   (size_t)file_size, file);
	fclose(file);
	if (bytes_read != (size_t)file_size) {
		fprintf(stderr, "host: short read on ROM '%s'\n", path);
		abort();
	}
	return (int)file_size;
}

/* ------------------------------------------------------------------ */
/* Framebuffer sink                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
	uint8_t *pixels;
	int width;
	int height;
	unsigned long frames;
	unsigned long regions;
	unsigned long long pixels_written;
} HostDisplay;

static void redraw(void *context, int left, int top, int width, int height)
{
	HostDisplay *display = context;

	if (!display || !display->pixels)
		return;
	if (width <= 0 || height <= 0)
		return;
	if (left < 0 || top < 0 || left + width > display->width ||
	    top + height > display->height)
		return;
	display->frames++;
	display->regions++;
	display->pixels_written += (unsigned long long)width * (unsigned)height;
}

/* ------------------------------------------------------------------ */
/* Audio service                                                        */
/* ------------------------------------------------------------------ */
/*
 * Mirror of the calculator frontend's audio pacing (source/winspire-ndless/
 * main.c). It is duplicated rather than shared because the two frontends have
 * different clocks: this one paces off the host monotonic clock, the CX one off
 * emulated guest cycles. What must stay identical is the *shape*: poll no more
 * often than once per millisecond of elapsed time, pull at most one mixer block
 * (PC_AUDIO_PULL_FRAMES) per pass, and drop rather than catch up beyond a
 * quarter second.
 *
 * The audio step is not free, so it is timed here: it is the only way to see
 * what the Sound Blaster DMA refill costs relative to pc_step().
 */
#define AUDIO_MIXER_HZ 44100U
#define AUDIO_SERVICE_US 1000U
#define AUDIO_MAX_BLOCKS_PER_CALL 16U
#define AUDIO_CATCHUP_LIMIT_FRAMES (AUDIO_MIXER_HZ / 4U)

static uint32_t audio_last_us;
static uint64_t audio_frame_accum;
static uint64_t audio_frames_serviced;
static uint64_t audio_active_blocks;
static uint64_t audio_samples_forwarded;
static unsigned long audio_calls;
static uint64_t audio_us_total;
/* int16_t for the 2-byte alignment the mixer requires on ARM; see the CX
 * frontend. uint8_t here would rely on the compiler aligning it anyway. */
static int16_t audio_buffer[PC_AUDIO_PULL_BYTES / 2];
static CxlinkAudioResampler audio_resampler;

/* ------------------------------------------------------------------ */
/* cxlink transport (host)                                              */
/* ------------------------------------------------------------------ */
/*
 * The calculator reaches the ESP32 over the dock UART; the host has no such
 * link. Instead it installs a sink transport that answers the handshake once
 * and then counts everything written to it, which drives the real code path --
 * handshake, framing, CRC, TX ring, audio blocks -- for a whole benchmark run
 * rather than only inside the self test. The byte counter is the evidence that
 * the datapath is live: it can only be non-zero if frames were framed, CRCs
 * computed and the ring drained.
 */
/*
 * The simulated bridge's replies are queued here and handed back by host_link_read(),
 * which makes this end behave like the real one for the two exchanges the host runs
 * into: it answers a keepalive PING (so a long benchmark run does not look like an
 * unplugged lead) and it applies the uplink credentials a NET_CONFIG carries, then
 * reports them back as held. The second one is what makes the provisioning path
 * observable end to end without hardware: the calculator half here is the real code,
 * the bridge half is a stub that only says whether it received what it was sent.
 */
#define HOST_REPLY_BYTES 4096u

typedef struct {
	/* Decodes what the calculator writes, so this end can answer it. */
	CxlinkDecoder decoder;
	uint8_t handshake[CXLINK_MAX_FRAME];
	size_t handshake_length;
	size_t handshake_sent;
	unsigned long long bytes_out;
	/* Replies synthesised so far, in order. */
	uint8_t reply[HOST_REPLY_BYTES];
	size_t reply_head;
	size_t reply_tail;
	/* What the "bridge" authenticated with, for one assertion at the end. */
	uint8_t ssid[CXLINK_WIFI_SSID_MAX];
	uint8_t password[CXLINK_WIFI_PASSWORD_MAX];
	bool credentials_applied;
	unsigned credential_frames;
	unsigned keepalives_answered;
} HostLink;

static HostLink host_link;

/* Set only when the config actually carries an ssid (see main). */
static const PCConfig *host_config;
static char provision_ssid_text[64];

static bool host_link_queue(HostLink *link, uint8_t type, uint8_t flags,
			   const uint8_t *payload, size_t length)
{
	uint8_t encoded[CXLINK_MAX_FRAME];
	size_t total = cxlink_encode(type, flags, 0, payload, length, encoded,
				     sizeof(encoded));

	if (!total || total > sizeof(link->reply) - link->reply_head)
		return false;
	memcpy(link->reply + link->reply_head, encoded, total);
	link->reply_head += total;
	return true;
}

static size_t host_link_write(void *context, const uint8_t *data, size_t length)
{
	HostLink *link = context;
	const CxlinkFrame *frame = NULL;
	size_t i;

	link->bytes_out += length;
	/*
	 * React to what the calculator sent, the way the firmware does. The
	 * decoder is the same one both real ends use, so anything this notices has
	 * already survived framing and CRC.
	 */
	for (i = 0; i < length; i++) {
		if (!cxlink_decoder_push(&link->decoder, data[i], &frame) || !frame)
			continue;
		if (frame->type == CXLINK_MSG_PING) {
			(void)host_link_queue(link, CXLINK_MSG_PONG, 0, NULL, 0);
			link->keepalives_answered++;
		} else if (frame->type == CXLINK_MSG_NET_CONFIG &&
			   frame->length >= sizeof(CxlinkNetConfig)) {
			CxlinkNetConfig config;
			CxlinkNetConfig report;

			memcpy(&config, frame->payload, sizeof(config));
			if (!config.ssid[0])
				continue;
			memcpy(link->ssid, config.ssid, sizeof(link->ssid));
			memcpy(link->password, config.password,
			       sizeof(link->password));
			link->credentials_applied = true;
			link->credential_frames++;
			/* The bridge now holds them and its "uplink" is up. */
			memset(&report, 0, sizeof(report));
			report.flags = CXLINK_NET_FLAG_UPLINK_UP |
				       CXLINK_NET_FLAG_CREDENTIALS;
			report.state = CXLINK_NET_STATE_CONNECTED;
			(void)host_link_queue(link, CXLINK_MSG_NET_CONFIG, 0,
					      (const uint8_t *)&report,
					      sizeof(report));
		}
	}
	return length;
}

static size_t host_link_read(void *context, uint8_t *data, size_t capacity)
{
	HostLink *link = context;
	size_t available = link->handshake_length - link->handshake_sent;

	if (!available) {
		available = link->reply_head - link->reply_tail;
		if (available > capacity)
			available = capacity;
		memcpy(data, link->reply + link->reply_tail, available);
		link->reply_tail += available;
		return available;
	}
	if (available > capacity)
		available = capacity;
	memcpy(data, link->handshake + link->handshake_sent, available);
	link->handshake_sent += available;
	return available;
}

static size_t host_link_pending(void *context)
{
	(void)context;
	return 0;
}

static void host_link_start(void)
{
	CxlinkHal hal;

	memset(&host_link, 0, sizeof(host_link));
	cxlink_decoder_init(&host_link.decoder);
	/* Pre-encode the peer's side of the handshake so the link comes up. */
	host_link.handshake_length = cxlink_encode(CXLINK_MSG_HELLO, 0, 0, NULL, 0,
						  host_link.handshake,
						  sizeof(host_link.handshake));
	hal.write = host_link_write;
	hal.read = host_link_read;
	hal.pending = host_link_pending;
	cxlink_init(&hal, &host_link);
}

/*
 * Hand the uplink credentials to the simulated bridge once, exactly as the
 * calculator frontend does. Everything after that - the retransmission until
 * the bridge confirms, and the re-send when it reports losing them - happens
 * inside cxlink.c.
 */
static void host_maybe_provision(void)
{
	static bool done;

	if (done || !host_config || !cxlink_link_up())
		return;
	done = true;
	snprintf(provision_ssid_text, sizeof(provision_ssid_text), "%s",
		 host_config->wifi_ssid);
	/* The pair was validated in main(), so this cannot fail here. */
	(void)cxlink_net_provision(host_config->wifi_ssid,
				   host_config->wifi_password);
}

static void host_audio_service(PC *pc)
{
	uint32_t now = get_uticks();
	uint32_t elapsed = now - audio_last_us;
	uint32_t frames;
	uint32_t blocks = 0;
	uint64_t scaled;
	uint64_t t0 = host_ns();
	unsigned int i;

	if (elapsed < AUDIO_SERVICE_US)
		return;
	audio_last_us = now;

	/* Same 1 ms gate as the calculator frontend: both the link and the
	 * audio path are serviced from here. */
	cxlink_poll();
	host_maybe_provision();

	scaled = (uint64_t)elapsed * AUDIO_MIXER_HZ + audio_frame_accum;
	frames = (uint32_t)(scaled / 1000000ULL);
	audio_frame_accum = scaled % 1000000ULL;
	if (frames > AUDIO_CATCHUP_LIMIT_FRAMES)
		frames = AUDIO_CATCHUP_LIMIT_FRAMES;

	while (frames && blocks < AUDIO_MAX_BLOCKS_PER_CALL) {
		uint32_t block = frames > PC_AUDIO_PULL_FRAMES ?
				 PC_AUDIO_PULL_FRAMES : frames;

		pc_audio_step(pc, (uint8_t *)audio_buffer, (int)(block * 4u));
		/* Report whether the mixer produced anything but silence: during
		 * a plain BIOS boot it is expected to be all zero. */
		for (i = 0; i < block * 2u; i++) {
			if (audio_buffer[i]) {
				audio_active_blocks++;
				break;
			}
		}
		if (cxlink_link_up())
			audio_samples_forwarded += cxlink_audio_resampler_feed(
				&audio_resampler, audio_buffer, block, false);
		audio_frames_serviced += block;
		frames -= block;
		blocks++;
	}
	audio_calls++;
	audio_us_total += host_ns() - t0;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

static void configure_defaults(PCConfig *config)
{
	memset(config, 0, sizeof(*config));
	config->mem_size = 16 * 1024 * 1024;
	config->vga_mem_size = 256 * 1024;
	config->width = DEFAULT_WIDTH;
	config->height = DEFAULT_HEIGHT;
	config->cpu_gen = 4;
	config->fpu = 0;
	config->clock_hz = 4770000U;
	config->vga_force_8dm = 1;
	config->bios = "source/winspire/bios.bin";
	config->vga_bios = "source/winspire/vgabios.bin";
}

/* ------------------------------------------------------------------ */
/* Benchmark / profile driver                                           */
/* ------------------------------------------------------------------ */

typedef struct {
	uint64_t target_instructions;	/* 0 = run until done or timeout */
	uint64_t wall_limit_ns;
	bool have_media;
	bool quiet;
	bool trace_phases;
} BenchOptions;

/* ------------------------------------------------------------------ */
/* cxlink self test                                                     */
/* ------------------------------------------------------------------ */
#ifdef CXLINK_ENABLE_SELFTEST

/*
 * Runs the shared bridge/protocol self test. This is the only executable check
 * of cxlink in the repository: it drives the codec, the reliable network
 * channel and the audio framing against an in-memory transport, without a
 * calculator or an ESP32.
 *
 * It also runs the ESP32's DHCP server (bridge_net.c). That code only ever
 * executes on a device that cannot be reached from here, so this is the one
 * place its behaviour is checked at all - including the lease bookkeeping, the
 * reply options and both checksums.
 */
static int run_selftest(void)
{
	if (!cxlink_selftest()) {
		fprintf(stderr, "cxlink self test: FAIL at cxlink.c:%d\n",
			cxlink_selftest_failure_line);
		return 1;
	}
	printf("cxlink self test: PASS (codec, ack/retry, audio framing, "
	       "corruption recovery, reset, wifi provisioning)\n");
#ifdef BRIDGE_NET_ENABLE_SELFTEST
	if (!bridge_net_selftest()) {
		fprintf(stderr, "bridge_net self test: FAIL at bridge_net.c:%d\n",
			bridge_net_selftest_failure_line);
		return 1;
	}
	printf("bridge_net self test: PASS (DHCP offer/ack/nak, lease expiry, "
	       "release, decline quarantine, checksums, frame routing)\n");
#endif
	return 0;
}

#else

static int run_selftest(void)
{
	fprintf(stderr, "host: built without CXLINK_ENABLE_SELFTEST\n");
	return 2;
}

#endif /* CXLINK_ENABLE_SELFTEST */

typedef struct {
	unsigned phase;
	uint64_t start_instructions;
	uint64_t start_ns;
	uint64_t end_instructions;
	uint64_t end_ns;
} PhaseRecord;

static double ns_to_seconds(uint64_t ns)
{
	return (double)ns / 1000000000.0;
}

static uint16_t phase_word(PC *pc)
{
	return *(volatile uint16_t *)(pc->phys_mem + PHASE_WORD_ADDR);
}

static int run_benchmark(PC *pc, HostDisplay *display, const BenchOptions *opts)
{
	uint64_t started = host_ns();
	uint64_t cpu_ns = 0;
	uint64_t vga_ns = 0;
	unsigned long cpu_calls = 0;
	unsigned long vga_calls = 0;
	uint64_t instructions = 0;
	PhaseRecord phases[MAX_PHASES];
	unsigned phase_count = 0;
	uint64_t post_instructions = 0;
	uint64_t post_ns = 0;
	bool done = false;
	bool timed_out = false;
	unsigned current_phase = 0xFFFFFFFFu;

	/*
	 * Phase timing must not include BIOS POST, which is largely halted while
	 * it waits on the emulated clock. Start counting at the payload's first
	 * marker write instead.
	 */
	started = host_ns();
	current_phase = 0xFFFFFFFFu;

	while (!done) {
		uint64_t t0 = host_ns();

		g_in_pc_step = 1;
		pc_step(pc);
		g_in_pc_step = 0;
		{
			uint64_t t1 = host_ns();
			cpu_ns += t1 - t0;
			cpu_calls++;
			t0 = t1;
		}
		pc_vga_step(pc);
		vga_ns += host_ns() - t0;
		vga_calls++;
		host_audio_service(pc);

		instructions = (uint64_t)cpui386_get_cycle(pc->cpu);
		g_instructions = (unsigned long long)instructions;

		if (opts->have_media) {
			unsigned word = phase_word(pc);

			g_phase = word;

			if (word != current_phase && word <= PHASE_DONE) {
				uint64_t now = host_ns();

				if (current_phase == 0xFFFFFFFFu) {
					/* First marker: the payload has started. */
					post_instructions = instructions;
					post_ns = now - started;
					phases[phase_count].phase = word;
					phases[phase_count].start_instructions = instructions;
					phases[phase_count].start_ns = now;
					current_phase = word;
					continue;
				}
				phases[phase_count].end_instructions = instructions;
				phases[phase_count].end_ns = now;
				current_phase = word;
				if (opts->trace_phases) {
					fprintf(stderr,
						"phase -> %u at %llu steps (%llu us guest time)\n",
						word,
						(unsigned long long)instructions,
						(unsigned long long)get_uticks());
					fflush(stderr);
				}
				/*
				 * The final marker is written *before* the payload
				 * halts, so stop once we have seen the last phase
				 * transition to done.
				 */
				if (word == PHASE_DONE) {
					/* phases[phase_count] was just completed. */
					phase_count++;
					done = true;
					break;
				}
				if (phase_count + 1 < MAX_PHASES) {
					phase_count++;
					phases[phase_count].phase = word;
					phases[phase_count].start_instructions =
						instructions;
					phases[phase_count].start_ns = now;
				}
			}
		} else if (opts->target_instructions &&
			   instructions >= opts->target_instructions) {
			done = true;
			break;
		}

		if (pc->shutdown_state == 8) {
			fprintf(stderr, "host: guest requested shutdown at %llu instructions\n",
				(unsigned long long)instructions);
			done = true;
			break;
		}
		if (host_ns() - started >= opts->wall_limit_ns) {
			timed_out = true;
			done = true;
			break;
		}
	}

	{
		uint64_t elapsed = host_ns() - started;
		double seconds = ns_to_seconds(elapsed);
		double ips = seconds > 0.0 ? (double)instructions / seconds : 0.0;

		printf("\n== emulation throughput ==\n");
		printf("instructions      : %llu\n",
		       (unsigned long long)instructions);
		printf("wall time         : %.3f s%s\n", seconds,
		       timed_out ? "  (wall-clock cap reached)" : "");
		printf("instructions/sec  : %.0f  (%.2f MIPS)\n",
		       ips, ips / 1000000.0);
		printf("vga redraw regions: %lu (%llu px)\n",
		       display->regions, display->pixels_written);
		if (post_instructions)
			printf("bios post         : %llu instructions in %.3f s\n",
			       (unsigned long long)post_instructions,
			       ns_to_seconds(post_ns));
		printf("cpu step          : %5.1f%% of wall  %lu calls  %.1f us/call\n",
		       elapsed ? 100.0 * (double)cpu_ns / (double)elapsed : 0.0,
		       cpu_calls,
		       cpu_calls ? (double)cpu_ns / (double)cpu_calls / 1000.0 : 0.0);
		printf("vga step          : %5.1f%% of wall  %lu calls  %.1f us/call\n",
		       elapsed ? 100.0 * (double)vga_ns / (double)elapsed : 0.0,
		       vga_calls,
		       vga_calls ? (double)vga_ns / (double)vga_calls / 1000.0 : 0.0);
		printf("audio step        : %5.1f%% of wall  %lu calls  %.1f us/call  "
		       "%llu mixer frames (%llu non-silent)\n",
		       elapsed ? 100.0 * (double)audio_us_total / (double)elapsed : 0.0,
		       audio_calls,
		       audio_calls ? (double)audio_us_total / (double)audio_calls / 1000.0
				   : 0.0,
		       (unsigned long long)audio_frames_serviced,
		       (unsigned long long)audio_active_blocks);
		printf("io bridge         : link %s, %llu link samples, "
		       "%llu frame bytes to transport\n",
		       cxlink_link_up() ? "up" : "down",
		       (unsigned long long)audio_samples_forwarded,
		       (unsigned long long)host_link.bytes_out);
		if (provision_ssid_text[0]) {
			CxlinkProvisionStatus prov;

			cxlink_net_provision_status(&prov);
			/*
			 * The whole point of this line: the credentials left the
			 * frontend, crossed the real framing/CRC/ring code, and arrived
			 * intact enough for a peer to act on - or did not, which the
			 * state says. The password is deliberately not printed.
			 */
			printf("wifi provision    : ssid \"%s\" %s, "
			       "%u credential frame(s), %s, "
			       "%u keepalive(s) answered\n",
			       provision_ssid_text,
			       host_link.credentials_applied ?
				       "applied by the simulated bridge" :
				       "NOT applied",
			       host_link.credential_frames,
			       prov.confirmed ? "confirmed" : "never confirmed",
			       host_link.keepalives_answered);
		}

		if (phase_count && !opts->quiet) {
			uint64_t payload_insns = 0;
			uint64_t payload_ns = 0;
			unsigned i;

			printf("\n== per-phase throughput ==\n");
			printf("%-3s %-38s %14s %10s %10s\n",
			       "ph", "instruction mix", "steps", "sec", "Msteps/s");
			for (i = 0; i < phase_count; i++) {
				uint64_t insns =
					phases[i].end_instructions -
					phases[i].start_instructions;
				uint64_t span_ns = phases[i].end_ns -
					phases[i].start_ns;
				const char *name = phases[i].phase < MAX_PHASES &&
					phase_names[phases[i].phase] ?
					phase_names[phases[i].phase] : "?";

				payload_insns += insns;
				payload_ns += span_ns;
				printf("%-3u %-38s %14llu %10.4f %10.2f\n",
				       phases[i].phase, name,
				       (unsigned long long)insns,
				       ns_to_seconds(span_ns),
				       span_ns ? (double)insns /
						ns_to_seconds(span_ns) / 1000000.0 : 0.0);
			}
			/*
			 * The headline figure above includes BIOS POST, which is mostly
			 * halted waiting on the emulated clock and is therefore not a
			 * measure of dispatch speed. This is the number to optimize and
			 * to quote in PERFORMANCE.md.
			 *
			 * Note: REP-prefixed opcodes retire in bulk, so "steps" counts
			 * emulator dispatch steps, not guest memory operations. The
			 * bulk-copy phases therefore show fewer steps per unit of real
			 * work than the register-only phases.
			 */
			printf("payload steady-state: %llu steps / %.3f s = %.2f Msteps/s\n",
			       (unsigned long long)payload_insns,
			       ns_to_seconds(payload_ns),
			       payload_ns ? (double)payload_insns /
					ns_to_seconds(payload_ns) / 1000000.0 : 0.0);
		}
	}
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--boot <img>] [--bench <millions>] [--seconds <s>]\n"
		"          [--ini <path>] [--quiet] [--trace-phases] [--selftest]\n",
		argv0);
}

int main(int argc, char **argv)
{
	PCConfig config;
	HostDisplay display;
	BenchOptions opts;
	PC *pc;
	uint8_t *framebuffer;
	int i;

	memset(&opts, 0, sizeof(opts));
	opts.wall_limit_ns = (uint64_t)DEFAULT_WALL_SECONDS * 1000000000ULL;
	install_crash_report();

	boot_ns = host_ns();
	configure_defaults(&config);

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--boot")) {
			const char *value = (i + 1 < argc) ? argv[++i] : NULL;

			if (!value) {
				usage(argv[0]);
				return 2;
			}
			config.fdd[0] = value;
			opts.have_media = true;
		} else if (!strcmp(argv[i], "--bench")) {
			const char *value = (i + 1 < argc) ? argv[++i] : NULL;

			if (!value) {
				usage(argv[0]);
				return 2;
			}
			opts.target_instructions =
				(uint64_t)strtod(value, NULL) * 1000000ULL;
		} else if (!strcmp(argv[i], "--seconds")) {
			const char *value = (i + 1 < argc) ? argv[++i] : NULL;

			if (!value) {
				usage(argv[0]);
				return 2;
			}
			opts.wall_limit_ns =
				(uint64_t)strtod(value, NULL) * 1000000000ULL;
		} else if (!strcmp(argv[i], "--ini")) {
			const char *value = (i + 1 < argc) ? argv[++i] : NULL;

			if (!value) {
				usage(argv[0]);
				return 2;
			}
			if (ini_parse(value, parse_conf_ini, &config) != 0) {
				fprintf(stderr, "host: cannot parse '%s'\n", value);
				return 1;
			}
		} else if (!strcmp(argv[i], "--quiet")) {
			opts.quiet = true;
		} else if (!strcmp(argv[i], "--trace-phases")) {
			opts.trace_phases = true;
		} else if (!strcmp(argv[i], "--selftest")) {
			/* Runs standalone: no PC, no BIOS, no media. */
			return run_selftest();
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	/*
	 * Same refusal the calculator frontend makes, for the same reason: a typo in
	 * the uplink credentials should be a sentence at startup, not a silent
	 * failure once the run is over. The check is the shared one, so anything
	 * accepted here is accepted by the bridge firmware too.
	 */
	if (config.wifi_ssid || config.wifi_password) {
		CxlinkWifiResult uplink = cxlink_wifi_check(config.wifi_ssid,
							   config.wifi_password);

		if (uplink != CXLINK_WIFI_OK) {
			fprintf(stderr, "host: [network] rejected: %s\n",
				cxlink_wifi_result_text(uplink));
			return 1;
		}
		if (config.wifi_ssid && config.wifi_ssid[0])
			host_config = &config;
	}

	if (!opts.have_media && !opts.target_instructions) {
		fprintf(stderr, "host: pass --boot <img> or --bench <millions>\n");
		return 2;
	}

	memset(&display, 0, sizeof(display));
	display.width = config.width;
	display.height = config.height;
	framebuffer = calloc(1, (size_t)config.width * config.height * (BPP / 8));
	if (!framebuffer) {
		fprintf(stderr, "host: out of memory for framebuffer\n");
		return 1;
	}
	display.pixels = framebuffer;

	printf("host frontend: %dx%d @ %d bpp, %ld MiB guest RAM, %ld KiB VGA, cpu_gen %d\n",
	       config.width, config.height, BPP, config.mem_size / (1024 * 1024),
	       config.vga_mem_size / 1024, config.cpu_gen);
	if (opts.have_media)
		printf("boot media     : %s\n", config.fdd[0] ?
		       config.fdd[0] : "(none)");

	pc = pc_new(redraw, &display, framebuffer, &config);
	load_bios_and_reset(pc);
	pc->boot_start_time = get_uticks();
	host_link_start();
	cxlink_audio_resampler_init(&audio_resampler);
	audio_last_us = get_uticks();
	if (opts.have_media) {
		/*
		 * Fresh RAM reads as zero, and zero is a valid phase number.
		 * Pre-load the phase word with a sentinel so the harness can tell
		 * "payload has not started yet" apart from "phase 0 is running".
		 */
		*(volatile uint16_t *)(pc->phys_mem + PHASE_WORD_ADDR) = 0xFFFF;
	}

	{
		int result = run_benchmark(pc, &display, &opts);

		pc_free_buffers(pc);
		free(framebuffer);
		return result;
	}
}
