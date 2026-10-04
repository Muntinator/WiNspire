#include <libndls.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pc.h"
#include "cxlink.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FRAMEBUFFER_BYTES \
	(SCREEN_WIDTH * SCREEN_HEIGHT * 2)
/* The rotated panel buffer holds the transposed guest surface. */
#define ROTATE_BUFFER_BYTES (FRAMEBUFFER_BYTES)
#ifndef TINY386_INPUT_POLL_LOOPS
/* Keep the proven sampling interval for the touchpad deadzone. */
#define TINY386_INPUT_POLL_LOOPS 2U
#endif
#ifndef TINY386_VIDEO_POLL_LOOPS
#define TINY386_VIDEO_POLL_LOOPS 1U
#endif
#define INPUT_POLL_LOOPS TINY386_INPUT_POLL_LOOPS
#define VIDEO_POLL_LOOPS TINY386_VIDEO_POLL_LOOPS
#if INPUT_POLL_LOOPS == 0 || \
	(INPUT_POLL_LOOPS & (INPUT_POLL_LOOPS - 1)) != 0
#error TINY386_INPUT_POLL_LOOPS must be a nonzero power of two
#endif
#if VIDEO_POLL_LOOPS == 0 || \
	(VIDEO_POLL_LOOPS & (VIDEO_POLL_LOOPS - 1)) != 0
#error TINY386_VIDEO_POLL_LOOPS must be a nonzero power of two
#endif
#define LCD_RETRY_MS 250ULL
#define LCD_POLL_MS 100ULL
#define POLL_MAX 1024U
#define CPU_HZ 4770000U
#define CPU_HZ_MIN 1000000U
#define CPU_HZ_MAX 100000000U
#define INS_CYCLES 12ULL
#define IDLE_INS 12288ULL
#define CURSOR_REG 0xC0000C00U
#define REDRAW_AREA \
	((SCREEN_WIDTH * SCREEN_HEIGHT * 3) / 4)
#define PAD_DEADZONE 2
#define PAD_SCALE 18
#define PAD_ACCEL_AT 80
#define PAD_ACCEL_SCALE 26
#define MOUSE_DELTA_MAX 96
#define BTN_LEFT 0x01
#define BTN_RIGHT 0x02
#define KEY_SEMICOLON 0x27
#define KEY_APOSTROPHE 0x34
#define KEY_ASTERISK 0x33
#define KEY_ALT 58
/*
 * Panel orientation corrections. ORIENT_FLIP_V reverses the rows and
 * ORIENT_FLIP_H the columns, so ORIENT_ROT_180 is both. See the comment on
 * transform_surface() for why this is a setting rather than a constant.
 */
#define ORIENT_FLIP_V 0x01
#define ORIENT_FLIP_H 0x02
#define ORIENT_IDENTITY 0x00
#define ORIENT_FLIP_TOP 0x01
#define ORIENT_FLIP_LEFT 0x02
#define ORIENT_ROT_180 (ORIENT_FLIP_V | ORIENT_FLIP_H)
#define ORIENT_DEFAULT ORIENT_ROT_180
#define KEY_APOSTROPHE 0x34
#define KEY_ASTERISK 0x33
#define KEY_ALT 58
#define GUEST_RAM_MIN (4L * 1024 * 1024)
#define GUEST_RAM_MAX (32L * 1024 * 1024)
#define VGA_RAM_MIN (64L * 1024)
#define VGA_RAM_MAX (1024L * 1024)

/*
 * Hardware profile.
 *
 * The original TI-Nspire CX and the CX II share the same ARM926EJ-S core and
 * the same 320x240-shaped guest surface, but differ in ways that the frontend
 * must not paper over:
 *
 *   - Panel orientation. CX units from hardware revision W and later, and all
 *     CX II units, mount the panel rotated, so lcd_type() reports
 *     SCR_240x320_565 instead of SCR_320x240_565. The guest still renders
 *     320x240, so the frontend rotates during the blit.
 *   - Hardware cursor. Only the CX II LCD controller exposes the cursor
 *     enable bit at 0xC0000C00. That address is not part of the Ndless API and
 *     must not be written on the original CX.
 *   - Core clock. The original CX responds to Ndless set_cpu_speed(); the CX II
 *     returns 0 for every CPU_SPEED_* value and is clocked through the PMU
 *     instead.
 *
 * The grayscale Clickpad/Touchpad hardware (PL110, 4bpp) is out of scope: its
 * panel cannot show the 16bpp guest surface.
 */
typedef struct {
	bool valid;
	bool cx2;             /* named cx2, not is_cx2, to avoid the libndls macro */
	bool rotated_panel;   /* panel is 240x320, guest surface must be rotated */
	bool has_hw_cursor;   /* CX II LCD controller cursor register exists */
	bool can_set_cpu_speed;
	uint16_t panel_width;
	uint16_t panel_height;
	scr_type_t panel_format;
} NspireHardware;

static NspireHardware hw;

/* Filled with a user-facing message when an early check fails. */
static char boot_error[256];

/*
 * Probe the running hardware. Returns false (with boot_error set) when the
 * panel cannot display the emulator's 16bpp output.
 */
static bool detect_hardware(void)
{
	scr_type_t format;

	memset(&hw, 0, sizeof(hw));
	if (is_classic) {
		snprintf(boot_error, sizeof(boot_error),
			 "This build needs a color TI-Nspire CX.\n"
			 "Grayscale Clickpad/Touchpad (PL110, 4bpp) is not supported.");
		return false;
	}
	hw.cx2 = is_cx2;
	hw.has_hw_cursor = is_cx2;
	/* set_cpu_speed() is a no-op on CX II; see its libndls implementation. */
	hw.can_set_cpu_speed = !is_cx2;
	format = lcd_type();
	switch (format) {
	case SCR_320x240_565:
		hw.panel_width = SCREEN_WIDTH;
		hw.panel_height = SCREEN_HEIGHT;
		hw.rotated_panel = false;
		break;
	case SCR_240x320_565:
		hw.panel_width = SCREEN_HEIGHT;
		hw.panel_height = SCREEN_WIDTH;
		hw.rotated_panel = true;
		break;
	default:
		snprintf(boot_error, sizeof(boot_error),
			 "Unsupported LCD layout (scr_type %d).\n"
			 "This build supports the 16bpp panels of the TI-Nspire CX "
			 "(320x240 and 240x320) and CX II.", (int)format);
		return false;
	}
	hw.panel_format = format;
	hw.valid = true;
	return true;
}

typedef struct {
	u8 *framebuffer;
	uint16_t *rotate_buffer; /* only allocated for a rotated panel */
	uint16_t *flip_buffer;   /* only allocated when the panel needs correcting */
	bool ready;
	bool lcd_active;
	bool dirty;
	bool full_redraw;
	int dirty_x;
	int dirty_y;
	int dirty_width;
	int dirty_height;
	uint64_t last_claim_ms;
	uint64_t last_draw_ms;
} Display;

/*
 * Present the guest surface through lcd_blit(), rotating when the panel is
 * mounted the other way up. Defined below with the rotation helper. The
 * forward declaration keeps the redraw path readable.
 */
static void panel_blit(Display *display);

typedef struct {
	const t_key *key;
	int keycode;
	bool is_pressed;
} KeyBinding;

typedef struct {
	bool has_position;
	uint16_t last_x;
	uint16_t last_y;
	int x_remainder;
	int y_remainder;
	int buttons;
	int arrows;
} TouchState;

#define TOUCH_ARROW_UP    (1 << 0)
#define TOUCH_ARROW_DOWN  (1 << 1)
#define TOUCH_ARROW_LEFT  (1 << 2)
#define TOUCH_ARROW_RIGHT (1 << 3)

static void *reserved_ram;
static size_t reserved_ram_size;
static void *reserved_vram;
static size_t reserved_vram_size;
static uint32_t guest_ticks;
static uint32_t last_cycle;
static uint64_t tick_remainder;
static uint32_t guest_hz = CPU_HZ;
static uint32_t input_poll_loops = INPUT_POLL_LOOPS;
static uint32_t video_poll_loops = VIDEO_POLL_LOOPS;
static int orientation = ORIENT_DEFAULT;
static bool orientation_marker;
static volatile bool mode_changed;
static TouchState touchpad_state;

static bool is_power_of_two(uint32_t value)
{
	return value && !(value & (value - 1));
}

static int parse_native_config(void *user, const char *section,
		const char *name, const char *value)
{
	if (!strcmp(section, "nspire")) {
		if (!strcmp(name, "input_poll_loops"))
			input_poll_loops = (uint32_t)strtoul(value, NULL, 0);
		else if (!strcmp(name, "video_poll_loops"))
			video_poll_loops = (uint32_t)strtoul(value, NULL, 0);
		else if (!strcmp(name, "orientation"))
			orientation = (int)strtol(value, NULL, 0);
		else if (!strcmp(name, "orientation_marker"))
			orientation_marker = strtol(value, NULL, 0) != 0;
		return 1;
	}
	return parse_conf_ini(user, section, name, value);
}

/*
 * Map physical Nspire keys to Linux input keycodes. The shared PS/2 layer
 * converts navigation keys to E0-prefixed PC set-1 scancodes. Touchpad arrow
 * zones use the same keycodes.
 */
static KeyBinding keys[] = {
	{ &KEY_NSPIRE_ESC, 1, false },
	{ &KEY_NSPIRE_1, 2, false },
	{ &KEY_NSPIRE_2, 3, false },
	{ &KEY_NSPIRE_3, 4, false },
	{ &KEY_NSPIRE_4, 5, false },
	{ &KEY_NSPIRE_5, 6, false },
	{ &KEY_NSPIRE_6, 7, false },
	{ &KEY_NSPIRE_7, 8, false },
	{ &KEY_NSPIRE_8, 9, false },
	{ &KEY_NSPIRE_9, 10, false },
	{ &KEY_NSPIRE_0, 11, false },
	{ &KEY_NSPIRE_MINUS, 12, false },
	{ &KEY_NSPIRE_EQU, 13, false },
	{ &KEY_NSPIRE_DEL, 14, false },
	{ &KEY_NSPIRE_TAB, 15, false },
	{ &KEY_NSPIRE_Q, 16, false },
	{ &KEY_NSPIRE_W, 17, false },
	{ &KEY_NSPIRE_E, 18, false },
	{ &KEY_NSPIRE_R, 19, false },
	{ &KEY_NSPIRE_T, 20, false },
	{ &KEY_NSPIRE_Y, 21, false },
	{ &KEY_NSPIRE_U, 22, false },
	{ &KEY_NSPIRE_I, 23, false },
	{ &KEY_NSPIRE_O, 24, false },
	{ &KEY_NSPIRE_P, 25, false },
	{ &KEY_NSPIRE_ENTER, 28, false },
	{ &KEY_NSPIRE_CTRL, 29, false },
	{ &KEY_NSPIRE_A, 30, false },
	{ &KEY_NSPIRE_S, 31, false },
	{ &KEY_NSPIRE_D, 32, false },
	{ &KEY_NSPIRE_F, 33, false },
	{ &KEY_NSPIRE_G, 34, false },
	{ &KEY_NSPIRE_H, 35, false },
	{ &KEY_NSPIRE_J, 36, false },
	{ &KEY_NSPIRE_K, 37, false },
	{ &KEY_NSPIRE_L, 38, false },
	{ &KEY_NSPIRE_SHIFT, 42, false },
	{ &KEY_NSPIRE_Z, 44, false },
	{ &KEY_NSPIRE_X, 45, false },
	{ &KEY_NSPIRE_C, 46, false },
	{ &KEY_NSPIRE_V, 47, false },
	{ &KEY_NSPIRE_B, 48, false },
	{ &KEY_NSPIRE_N, 49, false },
	{ &KEY_NSPIRE_M, 50, false },
	{ &KEY_NSPIRE_COMMA, 51, false },
	{ &KEY_NSPIRE_PERIOD, 52, false },
	{ &KEY_NSPIRE_DIVIDE, 53, false },
	{ &KEY_NSPIRE_VAR, KEY_SEMICOLON, false },
	{ &KEY_NSPIRE_MULTIPLY, KEY_ASTERISK, false },
	{ &KEY_NSPIRE_APOSTROPHE, KEY_APOSTROPHE, false },
	{ &KEY_NSPIRE_SPACE, 57, false },
	/*
	 * The CX has no Alt key at all, and Windows 95 needs one for every
	 * menu mnemonic and for Alt+Tab, so the hardware "menu" key - which
	 * has no other use in a PC guest - stands in for it.
	 */
	{ &KEY_NSPIRE_MENU, KEY_ALT, false },
	{ &KEY_NSPIRE_HOME, 102, false },
	{ &KEY_NSPIRE_UP, 103, false },
	{ &KEY_NSPIRE_LEFT, 105, false },
	{ &KEY_NSPIRE_RIGHT, 106, false },
	{ &KEY_NSPIRE_DOWN, 108, false },
};

/* Derive guest time from CPU cycles so host stalls do not cause timer jumps. */
uint32_t get_uticks(void)
{
	return guest_ticks;
}

static void reset_guest_timer(void)
{
	guest_ticks = 0;
	last_cycle = 0;
	tick_remainder = 0;
}

static void advance_guest_timer(PC *pc)
{
	uint32_t cycle = (uint32_t)cpui386_get_cycle(pc->cpu);
	uint64_t scaled_time;
	uint64_t elapsed;
	uint32_t instructions;

	instructions = cycle - last_cycle;
	/* Keep the PIT advancing while the guest waits in HLT. */
	if (!instructions)
		instructions = IDLE_INS;
	last_cycle = cycle;
	scaled_time = (uint64_t)instructions *
	                INS_CYCLES *
	                1000000ULL + tick_remainder;
	elapsed = scaled_time / guest_hz;
	tick_remainder = scaled_time % guest_hz;
	guest_ticks += (uint32_t)elapsed;
}

/* ------------------------------------------------------------------ */
/* CX <-> ESP32 I/O bridge service                                     */
/* ------------------------------------------------------------------ */
/*
 * Two jobs, both driven from the main loop rather than from inside the guest
 * interpreter:
 *
 *   - cxlink_poll() moves frames in and out of the dock UART (or does nothing
 *     when the build has no transport).
 *   - the audio path pulls a block out of the PC's shared mixer, converts it
 *     to what the link negotiated, and hands it to the bridge for the ESP32 to
 *     play.
 *
 * The audio pull is NOT optional when no ESP32 is attached. On this target
 * pc.c compiles the ISA DMA step out of pc_step() for speed, so the Sound
 * Blaster's buffer can only refill from here (see pc_audio_step()); skipping
 * the pull because the link is down would leave a guest audio driver waiting
 * forever for the interrupt that a completed DMA block is supposed to raise.
 *
 * Both are paced against guest time rather than loop iterations, because one
 * batch can be tens of milliseconds of guest time depending on
 * TINY386_PC_STEP_COUNT in cx_profiles.h.
 */
#define BRIDGE_POLL_US 1000U
/* The rate sb16.c and adlib.c mix at. Not configurable in this tree. */
#define AUDIO_MIXER_HZ 44100U
/* Most mixer blocks to pull in one call; a longer backlog is dropped. */
#define AUDIO_MAX_BLOCKS_PER_CALL 16U
/* Never try to catch up more than this after a long stall. */
#define AUDIO_CATCHUP_LIMIT_FRAMES (AUDIO_MIXER_HZ / 4U)

static uint32_t bridge_last_us;
/* Fractional mixer-frame remainder, in units of 1/AUDIO_MIXER_HZ frame. */
static uint64_t audio_frame_accum;
/* Resampling and block buffering for the link; see cxlink.h. */
static CxlinkAudioResampler audio_resampler;
static uint32_t audio_frames_dropped;
static uint32_t audio_samples_sent;
/*
 * int16_t rather than uint8_t so the buffer is guaranteed 2-byte aligned:
 * mixer_callback() and sb16_audio_callback() both read it as a stereo sample
 * array, and the ARM926 faults on an unaligned halfword access.
 */
static int16_t mixer_buffer[PC_AUDIO_PULL_BYTES / 2];

/*
 * Uplink provisioning, driven from the config file and pushed to the bridge
 * once the link is up. `provision_done` keeps the hand-off to one call: from
 * then on cxlink retransmits the credentials itself until the bridge confirms
 * them, and re-sends them by itself if the bridge is restarted, so the frontend
 * never has to poll for it.
 */
static bool provision_done;
static bool provision_note_valid;
static char provision_note[128];
/* The config asked for an uplink, whether or not a bridge ever answered. */
static bool uplink_configured;
/* Set by main() before the loop: the bridge needs the [network] section. */
static const PCConfig *bridge_config;

/*
 * Called once the bridge is answering. Returns false when there is nothing to
 * do (no SSID configured); the result of a failed check is reported at exit in
 * provision_note, because a message box here would sit on top of the emulator.
 */
static bool provision_uplink(const PCConfig *config)
{
	CxlinkWifiResult result;

	if (!config->wifi_ssid || !config->wifi_ssid[0])
		return false;
	result = cxlink_net_provision(config->wifi_ssid, config->wifi_password);
	provision_note_valid = true;
	if (result == CXLINK_WIFI_OK) {
		/*
		 * The SSID is not a secret and naming it is how a user sees that the
		 * right network was configured. The password is never in here.
		 */
		snprintf(provision_note, sizeof(provision_note),
			 "Wi-Fi: SSID \"%s\" sent to the ESP32.",
			 config->wifi_ssid);
	} else {
		snprintf(provision_note, sizeof(provision_note),
			 "Wi-Fi: [network] entry was not sent - %s.",
			 cxlink_wifi_result_text(result));
	}
	return result == CXLINK_WIFI_OK;
}

static void io_bridge_reset(void)
{
	bridge_last_us = get_uticks();
	audio_frame_accum = 0;
	audio_frames_dropped = 0;
	audio_samples_sent = 0;
	cxlink_audio_resampler_init(&audio_resampler);
}

/*
 * Service the Sound Blaster and forward one mixer block.
 *
 * pc_audio_step() always runs, even with no link: see the note above on why the
 * DMA refill cannot be skipped. Conversion and framing only happen when there
 * is somewhere to send the samples.
 */
static void io_bridge_audio_block(PC *pc, uint32_t frames)
{
	pc_audio_step(pc, (uint8_t *)mixer_buffer, (int)(frames * 4u));
	if (!cxlink_link_up())
		return;
	audio_samples_sent += cxlink_audio_resampler_feed(&audio_resampler,
							  mixer_buffer, frames, false);
}

static void service_io_bridge(PC *pc)
{
	uint32_t now = get_uticks();
	uint32_t elapsed = now - bridge_last_us;
	uint32_t frames;
	uint32_t blocks = 0;
	uint64_t scaled;

	if (elapsed < BRIDGE_POLL_US)
		return;
	bridge_last_us = now;

	cxlink_poll();

	/* Hand the uplink credentials over as soon as the bridge is talking. */
	if (!provision_done && cxlink_link_up()) {
		provision_done = true;
		(void)provision_uplink(bridge_config);
	}

	/*
	 * Fixed-point accumulation: a 1 ms poll interval is 44.1 mixer frames,
	 * so truncating the fraction each poll would run audio ~2% slow.
	 */
	scaled = (uint64_t)elapsed * AUDIO_MIXER_HZ + audio_frame_accum;
	frames = (uint32_t)(scaled / 1000000ULL);
	audio_frame_accum = scaled % 1000000ULL;

	if (frames > AUDIO_CATCHUP_LIMIT_FRAMES) {
		/* Long stall: drop the backlog instead of compounding it. */
		audio_frames_dropped += frames - AUDIO_CATCHUP_LIMIT_FRAMES;
		frames = AUDIO_CATCHUP_LIMIT_FRAMES;
	}

	while (frames && blocks < AUDIO_MAX_BLOCKS_PER_CALL) {
		uint32_t block = frames;

		if (block > PC_AUDIO_PULL_FRAMES)
			block = PC_AUDIO_PULL_FRAMES;
		io_bridge_audio_block(pc, block);
		frames -= block;
		blocks++;
	}
	/* Anything the block cap left behind is a deliberate drop. */
	audio_frames_dropped += frames;
}

/*
 * Reserve guest and VGA RAM before smaller allocations. bigmalloc() consumes
 * these blocks when pc_new() allocates emulator memory, avoiding Ndless heap
 * fragmentation during boot.
 */
void *bigmalloc(size_t size)
{
	void *memory;

	if (reserved_ram && size == reserved_ram_size) {
		memory = reserved_ram;
		reserved_ram = NULL;
		reserved_ram_size = 0;
		return memory;
	}
	if (reserved_vram && size == reserved_vram_size) {
		memory = reserved_vram;
		reserved_vram = NULL;
		reserved_vram_size = 0;
		return memory;
	}
	memory = calloc(1, size);
	if (!memory) {
		abort();
	}
	return memory;
}

static bool reserve_guest_memory(const PCConfig *config)
{
	/* Keep the configured RAM size; silently shrinking it can break the guest. */
	reserved_ram = calloc(1, config->mem_size);
	if (!reserved_ram)
		return false;
	reserved_ram_size = config->mem_size;
	return true;
}

static bool reserve_vga_memory(const PCConfig *config)
{
	reserved_vram = calloc(1, config->vga_mem_size);
	if (!reserved_vram) {
		return false;
	}
	reserved_vram_size = config->vga_mem_size;
	return true;
}

static void free_reserved_memory(void)
{
	free(reserved_ram);
	reserved_ram = NULL;
	reserved_ram_size = 0;
	free(reserved_vram);
	reserved_vram = NULL;
	reserved_vram_size = 0;
}

/* Run file checks before LCD takeover so TI-OS can display failures. */
static bool preflight_file(const char *label, const char *path, const char *mode,
			   long max_size)
{
	FILE *file;
	long file_size;

	if (!path || !path[0]) {
		snprintf(boot_error, sizeof(boot_error),
			 "Set %s in [pc] to a file path.", label);
		return false;
	}
	file = fopen(path, mode);
	if (!file) {
		snprintf(boot_error, sizeof(boot_error),
			 "Cannot open %s for %s:\n%s\nCheck the path and permissions.",
			 label, !strcmp(mode, "rb") ? "reading" : "reading and writing", path);
		return false;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) <= 0) {
		snprintf(boot_error, sizeof(boot_error),
			 "Empty or unreadable %s:\n%s.",
			 label, path);
		fclose(file);
		return false;
	}
	fclose(file);
	if (max_size > 0 && file_size > max_size) {
		snprintf(boot_error, sizeof(boot_error),
			 "%s exceeds the %ld-byte limit:\n%s", label, max_size, path);
		return false;
	}
	return true;
}

static bool preflight_boot_files(const PCConfig *config)
{
	unsigned int index;

	if (!preflight_file("bios", config->bios, "rb", 0x100000L))
		return false;
	if (!preflight_file("vga_bios", config->vga_bios, "rb",
			    config->mem_size - 0xc0000L))
		return false;
	if ((!config->disks[0] || !config->disks[0][0]) &&
	    (!config->fdd[0] || !config->fdd[0][0])) {
		snprintf(boot_error, sizeof(boot_error),
			 "Set hda or fda in [pc] to your boot disk image.");
		return false;
	}
	for (index = 0;
	     index < sizeof(config->disks) / sizeof(config->disks[0]); index++) {
		if (config->disks[index] && config->disks[index][0] &&
		    !preflight_file("disk image", config->disks[index], "r+b", 0))
			return false;
	}
	for (index = 0;
	     index < sizeof(config->fdd) / sizeof(config->fdd[0]); index++) {
		if (config->fdd[index] && config->fdd[index][0] &&
		    !preflight_file("floppy image", config->fdd[index], "r+b", 0))
			return false;
	}
	return true;
}

int load_rom(void *guest_memory, const char *path, uword address, int backward)
{
	FILE *file = fopen(path, "rb");
	long file_size;
	size_t bytes_read;

	if (!file) {
		abort();
	}
	if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0) {
		fclose(file);
		abort();
	}
	rewind(file);
	if (backward)
		bytes_read = fread((uint8_t *)guest_memory + address - file_size, 1, file_size, file);
	else
		bytes_read = fread((uint8_t *)guest_memory + address, 1, file_size, file);
	fclose(file);
	if (bytes_read != (size_t)file_size) {
		abort();
	}
	return (int)file_size;
}

static uint64_t host_millis(void)
{
	return (uint64_t)(((unsigned long)clock() * 1000UL) / CLOCKS_PER_SEC);
}

/*
 * The CX II LCD controller has a hardware cursor overlay that would draw over
 * the emulated screen. The register is not exposed by the Ndless API, so it is
 * only touched on the hardware that has it.
 */
static void disable_os_cursor(void)
{
	volatile uint32_t *cursor_reg;

	if (!hw.has_hw_cursor)
		return;
	cursor_reg = (volatile uint32_t *)CURSOR_REG;
	*cursor_reg &= ~1U;
}

void vga_mode_changed(void)
{
	mode_changed = true;
}

/*
 * Ndless can return LCD ownership to TI-OS during launch and modal transitions.
 * Reclaim it and disable the TI-OS cursor overlay before drawing.
 */
static bool claim_lcd(Display *display,
		bool force, uint64_t now, bool *reclaimed)
{
	if (force || !display->lcd_active ||
	    !display->last_claim_ms ||
	    now - display->last_claim_ms >=
		    LCD_RETRY_MS) {
		disable_os_cursor();
		display->lcd_active = lcd_init(SCR_320x240_565);
		display->last_claim_ms = now;
		if (reclaimed)
			*reclaimed = display->lcd_active;
	} else {
		disable_os_cursor();
		if (reclaimed)
			*reclaimed = false;
	}
	return display->lcd_active;
}

static void draw_frame(Display *display, bool force)
{
	uint64_t now = host_millis();

	if (!display->ready)
		return;
	if (!claim_lcd(display, force, now, NULL))
		return;
	panel_blit(display);
	display->last_draw_ms = now;
}

static void transform_surface(const uint16_t *src, uint16_t *dst, int op);

static void draw_region(Display *display,
		int left, int top, int width, int height)
{
	uint64_t now = host_millis();
	const uint16_t *source;
	uint16_t *screen;
	int row;

	/* The redraw queue clips bounds before this function copies any pixels. */
	if (width * height >= REDRAW_AREA) {
		draw_frame(display, false);
		return;
	}
	/*
	 * Clip again rather than trusting the caller. Every branch below
	 * computes panel coordinates by reflecting the guest rectangle, and an
	 * unclipped one writes outside both the staging buffer and the panel -
	 * which is how v1.0.2 ended up reading 240 rows past the end of both.
	 */
	if (left < 0) {
		width += left;
		left = 0;
	}
	if (top < 0) {
		height += top;
		top = 0;
	}
	if (left + width > SCREEN_WIDTH)
		width = SCREEN_WIDTH - left;
	if (top + height > SCREEN_HEIGHT)
		height = SCREEN_HEIGHT - top;
	if (width <= 0 || height <= 0)
		return;
	/*
	 * A rotated panel has no linear 320-wide surface to patch in place, and
	 * REAL_SCREEN_BASE_ADDRESS strides by the panel width. Present the whole
	 * frame through the rotating blit instead.
	 */
	if (hw.rotated_panel) {
		draw_frame(display, false);
		return;
	}
	/*
	 * The partial update has to be written to the panel through the same
	 * mapping the full-frame blit uses, or the two paths disagree and the
	 * screen tears into a mixture of orientations.
	 *
	 * The band is staged in flip_buffer and then copied out. Both sides of
	 * that copy have to run in the SAME direction: the staging loop writes
	 * flip rows mirror, mirror-1, ... (going *up* the panel as the guest
	 * row increases when ORIENT_FLIP_V is set), so the copy has to start
	 * at the lowest row that was written - mirror - height + 1 - and walk
	 * upward. Starting it at mirror instead, as an earlier version did,
	 * read height-1 rows that had never been written and presented stale
	 * pixels, which made the screen unreadable.
	 *
	 * With ORIENT_FLIP_H the columns reverse too, so the staging copy is
	 * per-pixel rather than a memcpy: guest column left lands at panel
	 * column SCREEN_WIDTH - 1 - left, and the band runs backwards from
	 * there.
	 *
	 * The orientation marker is a full-frame overlay, so a partial update
	 * would erase it. Draw the frame instead whenever it is enabled.
	 */
	if (orientation_marker) {
		draw_frame(display, false);
		return;
	}
	if (!claim_lcd(display, false, now, NULL))
		return;
	if (display->flip_buffer && orientation != ORIENT_IDENTITY) {
		int flip_rows = (orientation & ORIENT_FLIP_V) != 0;
		int flip_cols = (orientation & ORIENT_FLIP_H) != 0;
		int mirror = SCREEN_HEIGHT - 1 - top;
		int first = flip_rows ? mirror - height + 1 : top;
		int col = flip_cols ? SCREEN_WIDTH - left - width : left;

		for (row = 0; row < height; row++) {
			const uint16_t *in =
				(const uint16_t *)display->framebuffer +
				(size_t)(top + row) * SCREEN_WIDTH + left;
			uint16_t *out = (uint16_t *)display->flip_buffer +
				(size_t)(flip_rows ? mirror - row : top + row) *
				SCREEN_WIDTH + col;
			int i;

			if (flip_cols) {
				for (i = 0; i < width; i++)
					out[i] = in[width - 1 - i];
			} else {
				memcpy(out, in,
				       (size_t)width * sizeof(uint16_t));
			}
		}
		source = display->flip_buffer +
			(size_t)first * SCREEN_WIDTH + col;
		screen = (uint16_t *)REAL_SCREEN_BASE_ADDRESS +
			(size_t)first * SCREEN_WIDTH + col;
		if (!flip_rows) {
			/* Rows stay in place, so copy them one at a time. */
			for (row = 0; row < height; row++) {
				memcpy(screen + (size_t)row * SCREEN_WIDTH,
				       source + (size_t)row * SCREEN_WIDTH,
				       (size_t)width * sizeof(uint16_t));
			}
			display->last_draw_ms = now;
			return;
		}
	} else {
		source = (const uint16_t *)display->framebuffer +
			top * SCREEN_WIDTH + left;
		screen = (uint16_t *)REAL_SCREEN_BASE_ADDRESS +
			top * SCREEN_WIDTH + left;
	}
	if (width == SCREEN_WIDTH) {
		memcpy(screen, source,
			(size_t)height * SCREEN_WIDTH * sizeof(uint16_t));
		display->last_draw_ms = now;
		return;
	}
	for (row = 0; row < height; row++) {
		memcpy(screen, source, (size_t)width * sizeof(uint16_t));
		source += SCREEN_WIDTH;
		screen += SCREEN_WIDTH;
	}
	display->last_draw_ms = now;
}

static void queue_redraw(Display *display,
		int left, int top, int width, int height)
{
	int right;
	int bottom;

	if (!display->ready)
		return;
	if (left <= 0 && top <= 0 && width >= SCREEN_WIDTH &&
	    height >= SCREEN_HEIGHT) {
		display->full_redraw = true;
		display->dirty_x = 0;
		display->dirty_y = 0;
		display->dirty_width = SCREEN_WIDTH;
		display->dirty_height = SCREEN_HEIGHT;
	} else {
		if (left < 0) {
			width += left;
			left = 0;
		}
		if (top < 0) {
			height += top;
			top = 0;
		}
		if (left >= SCREEN_WIDTH ||
		    top >= SCREEN_HEIGHT ||
		    width <= 0 || height <= 0)
			return;
		if (left + width > SCREEN_WIDTH)
			width = SCREEN_WIDTH - left;
		if (top + height > SCREEN_HEIGHT)
			height = SCREEN_HEIGHT - top;
		if (display->full_redraw)
			return;
		if (!display->dirty) {
			display->dirty_x = left;
			display->dirty_y = top;
			display->dirty_width = width;
			display->dirty_height = height;
		} else {
			right = display->dirty_x + display->dirty_width;
			bottom = display->dirty_y + display->dirty_height;
			if (left < display->dirty_x)
				display->dirty_x = left;
			if (top < display->dirty_y)
				display->dirty_y = top;
			if (left + width > right)
				right = left + width;
			if (top + height > bottom)
				bottom = top + height;
			display->dirty_width = right - display->dirty_x;
			display->dirty_height = bottom - display->dirty_y;
		}
	}
	display->dirty = true;
}

/*
 * Merge VGA callbacks into one LCD update per video poll.
 */
static void flush_redraw(Display *display)
{
	if (!display->dirty)
		return;
	display->dirty = false;
	if (display->full_redraw)
		draw_frame(display, false);
	else
		draw_region(display, display->dirty_x,
				       display->dirty_y, display->dirty_width,
				       display->dirty_height);
	display->full_redraw = false;
}

static void keep_lcd(Display *display)
{
	uint64_t now = host_millis();
	bool reclaimed = false;

	if (mode_changed)
		return;
	if (!display->last_draw_ms ||
	    now - display->last_draw_ms >=
		    LCD_POLL_MS) {
		if (claim_lcd(display, false, now,
					    &reclaimed) &&
		    reclaimed) {
			panel_blit(display);
			display->last_draw_ms = now;
		}
	}
}

static void redraw(void *context,
		int left, int top, int width, int height)
{
	Display *display = context;

	/* Take LCD ownership on the first VGA redraw, regardless of firmware. */
	if (!display->ready) {
		display->ready = true;
		mode_changed = false;
		draw_frame(display, true);
	} else if (mode_changed) {
		mode_changed = false;
		queue_redraw(display, 0, 0, SCREEN_WIDTH,
				     SCREEN_HEIGHT);
	} else {
		queue_redraw(display, left, top, width, height);
	}
}

static uint32_t hide_os_cursor(void)
{
	volatile uint32_t *cursor_reg;
	uint32_t saved_cursor;

	if (!hw.has_hw_cursor)
		return 0;
	cursor_reg = (volatile uint32_t *)CURSOR_REG;
	saved_cursor = *cursor_reg;
	*cursor_reg = saved_cursor & ~1U;
	return saved_cursor;
}

static void restore_os_cursor(uint32_t saved_cursor)
{
	volatile uint32_t *cursor_reg;

	if (!hw.has_hw_cursor)
		return;
	cursor_reg = (volatile uint32_t *)CURSOR_REG;
	*cursor_reg = saved_cursor;
}

/*
 * Present the 320x240 guest surface through lcd_blit().
 *
 * A rotated panel (240x320) cannot accept the guest surface directly, so the
 * frames are transposed once per blit. The transpose is O(pixels) with a
 * sequential read and a column-strided write, which is the cheapest correct
 * option without a rotating LCD controller mode.
 *
 * WINSPIRE_PANEL_ROTATE_CCW selects which way that transposition runs, for
 * units whose panel is mounted the other way round. It is a mount-direction
 * choice only; it cannot correct a mirror, because a mirror is not a
 * rotation. The 180-degree correction the original CX needs is handled by
 * rotate_surface_180() below.
 */
static void rotate_surface(const uint16_t *src, uint16_t *dst)
{
	int x;
	int y;

#ifdef WINSPIRE_PANEL_ROTATE_CCW
	for (y = 0; y < SCREEN_HEIGHT; y++) {
		for (x = 0; x < SCREEN_WIDTH; x++)
			dst[(SCREEN_WIDTH - 1 - x) * SCREEN_HEIGHT + y] =
				src[y * SCREEN_WIDTH + x];
	}
#else
	for (y = 0; y < SCREEN_HEIGHT; y++) {
		for (x = 0; x < SCREEN_WIDTH; x++)
			dst[x * SCREEN_HEIGHT + (SCREEN_HEIGHT - 1 - y)] =
				src[y * SCREEN_WIDTH + x];
	}
#endif
}

/*
 * Panel orientation.
 *
 * Four corrections are possible, and which one a unit needs is a property of
 * how that unit's LCD is mounted - it cannot be read from the Ndless API. It
 * has also proven impossible to derive reliably from documentation: the
 * TI-Nspire OS's bottom-left drawing origin describes how the OS issues
 * drawing commands, not the order in which lcd_blit() fills panel memory, and
 * reading it as a scan order is what produced two wrong releases in a row.
 *
 * So the correction is a runtime setting (`orientation` under `[nspire]` in
 * winspire.ini) rather than something compiled in, and `orientation_marker`
 * draws corner brackets whose leg lengths identify which way is up. Both
 * together settle it from a single photograph instead of from an argument.
 *
 * The flag bits compose: ORIENT_FLIP_V reverses the rows, ORIENT_FLIP_H
 * reverses the columns.
 */
static void transform_surface(const uint16_t *src, uint16_t *dst, int op)
{
	int row;

	for (row = 0; row < SCREEN_HEIGHT; row++) {
		int sy = (op & ORIENT_FLIP_V) ?
			SCREEN_HEIGHT - 1 - row : row;
		const uint16_t *in = src + (size_t)sy * SCREEN_WIDTH;
		uint16_t *out = dst + (size_t)row * SCREEN_WIDTH;

		if (op & ORIENT_FLIP_H) {
			int col;

			for (col = 0; col < SCREEN_WIDTH; col++)
				out[col] = in[SCREEN_WIDTH - 1 - col];
		} else {
			memcpy(out, in, (size_t)SCREEN_WIDTH * sizeof(uint16_t));
		}
	}
}

/*
 * Corner brackets drawn on top of the presented surface. Each corner gets a
 * different pair of leg lengths, so one photograph identifies which corner is
 * which and therefore exactly which correction the unit needs.
 */
static void draw_orientation_marker(uint16_t *surface, int op)
{
	static const struct {
		int x;
		int y;
		int dx;
		int dy;
		int h;
		int v;
	} corners[4] = {
		{ 0,                   0,                   1, 1, 28, 20 },
		{ SCREEN_WIDTH - 1,    0,                  -1, 1, 12, 20 },
		{ 0,                   SCREEN_HEIGHT - 1,   1, -1, 28, 10 },
		{ SCREEN_WIDTH - 1,    SCREEN_HEIGHT - 1,  -1, -1, 12, 10 },
	};
	unsigned int index;

	for (index = 0; index < 4; index++) {
		int px = (op & ORIENT_FLIP_H) ?
			SCREEN_WIDTH - 1 - corners[index].x : corners[index].x;
		int py = (op & ORIENT_FLIP_V) ?
			SCREEN_HEIGHT - 1 - corners[index].y : corners[index].y;
		int step;
		int dx = (op & ORIENT_FLIP_H) ? -corners[index].dx : corners[index].dx;
		int dy = (op & ORIENT_FLIP_V) ? -corners[index].dy : corners[index].dy;

		for (step = 0; step < corners[index].h; step++) {
			int x = px + dx * step;

			surface[(size_t)py * SCREEN_WIDTH + x] = 0xFFFF;
			surface[(size_t)(py + dy) * SCREEN_WIDTH + x] = 0xFFFF;
		}
		for (step = 0; step < corners[index].v; step++) {
			int y = py + dy * step;

			surface[(size_t)y * SCREEN_WIDTH + px] = 0xFFFF;
			surface[(size_t)y * SCREEN_WIDTH + px + dx] = 0xFFFF;
		}
	}
}

static void panel_blit(Display *display)
{
	if (!display->framebuffer)
		return;
	if (hw.rotated_panel) {
		if (!display->rotate_buffer)
			return;
		rotate_surface((const uint16_t *)display->framebuffer,
			       display->rotate_buffer);
		lcd_blit(display->rotate_buffer, hw.panel_format);
		return;
	}
	if (display->flip_buffer) {
		transform_surface((const uint16_t *)display->framebuffer,
				  display->flip_buffer, orientation);
		if (orientation_marker)
			draw_orientation_marker(display->flip_buffer,
						orientation);
		lcd_blit(display->flip_buffer, hw.panel_format);
		return;
	}
	lcd_blit(display->framebuffer, hw.panel_format);
}

static int clamp_int(int value, int min_value, int max_value)
{
	if (value < min_value)
		return min_value;
	if (value > max_value)
		return max_value;
	return value;
}

static int scale_mouse_delta(int delta, int *remainder)
{
	int sign = delta < 0 ? -1 : 1;
	int magnitude = delta < 0 ? -delta : delta;
	int accelerated;
	int accumulated;
	int scaled;

	if (magnitude <= PAD_DEADZONE)
		return 0;
	magnitude -= PAD_DEADZONE;
	accelerated = magnitude;
	if (magnitude >= PAD_ACCEL_AT)
		accelerated += (magnitude * PAD_SCALE) /
				PAD_ACCEL_SCALE;
	accumulated = *remainder + sign * accelerated;
	scaled = accumulated / PAD_SCALE;
	*remainder = accumulated % PAD_SCALE;
	return clamp_int(scaled, -MOUSE_DELTA_MAX, MOUSE_DELTA_MAX);
}

static int touchpad_arrow_mask(unsigned char arrow)
{
	switch (arrow) {
	case TPAD_ARROW_UP: return TOUCH_ARROW_UP;
	case TPAD_ARROW_UPRIGHT: return TOUCH_ARROW_UP | TOUCH_ARROW_RIGHT;
	case TPAD_ARROW_RIGHT: return TOUCH_ARROW_RIGHT;
	case TPAD_ARROW_RIGHTDOWN: return TOUCH_ARROW_RIGHT | TOUCH_ARROW_DOWN;
	case TPAD_ARROW_DOWN: return TOUCH_ARROW_DOWN;
	case TPAD_ARROW_DOWNLEFT: return TOUCH_ARROW_DOWN | TOUCH_ARROW_LEFT;
	case TPAD_ARROW_LEFT: return TOUCH_ARROW_LEFT;
	case TPAD_ARROW_LEFTUP: return TOUCH_ARROW_LEFT | TOUCH_ARROW_UP;
	default: return 0;
	}
}

static void update_touch_arrows(PC *pc, int new_arrows)
{
	static const struct {
		int mask;
		int keycode;
	} arrow_keys[] = {
		{ TOUCH_ARROW_UP, 103 },
		{ TOUCH_ARROW_DOWN, 108 },
		{ TOUCH_ARROW_LEFT, 105 },
		{ TOUCH_ARROW_RIGHT, 106 },
	};
	unsigned int index;

	for (index = 0;
	     index < sizeof(arrow_keys) / sizeof(arrow_keys[0]); index++) {
		bool was_pressed = (touchpad_state.arrows & arrow_keys[index].mask) != 0;
		bool is_pressed = (new_arrows & arrow_keys[index].mask) != 0;

		if (was_pressed != is_pressed)
			ps2_put_keycode(pc->kbd, is_pressed, arrow_keys[index].keycode);
	}
	touchpad_state.arrows = new_arrows;
}

/*
 * Convert the calculator touchpad into PS/2 relative mouse packets. Fractional
 * remainders make slow drags smooth, while the acceleration threshold still
 * allows full-screen movement without excessive finger travel.
 */
static void poll_touchpad_mouse(PC *pc)
{
	touchpad_report_t report;
	bool touching;
	int buttons = 0;
	int arrows = 0;
	int dx = 0;
	int dy = 0;

	if (!is_touchpad || !pc->mouse)
		return;
	/* Release held inputs after a failed scan so keys and clicks cannot stick. */
	if (touchpad_scan(&report) != 0) {
		update_touch_arrows(pc, 0);
		if (touchpad_state.buttons) {
			ps2_mouse_event(pc->mouse, 0, 0, 0, 0);
			touchpad_state.buttons = 0;
		}
		touchpad_state.has_position = false;
		touchpad_state.x_remainder = 0;
		touchpad_state.y_remainder = 0;
		return;
	}
	touching = report.contact || report.pressed;
	if (report.pressed)
		arrows = touchpad_arrow_mask(report.arrow);
	if (report.pressed && !arrows)
		buttons |= isKeyPressed(KEY_NSPIRE_CTRL) ?
			BTN_RIGHT : BTN_LEFT;
	update_touch_arrows(pc, arrows);
	if (touching && touchpad_state.has_position) {
		dx = scale_mouse_delta((int)report.x - (int)touchpad_state.last_x,
				       &touchpad_state.x_remainder);
		dy = scale_mouse_delta((int)touchpad_state.last_y - (int)report.y,
				       &touchpad_state.y_remainder);
	}
	if (touching) {
		touchpad_state.last_x = report.x;
		touchpad_state.last_y = report.y;
		touchpad_state.has_position = true;
	} else {
		touchpad_state.has_position = false;
		touchpad_state.x_remainder = 0;
		touchpad_state.y_remainder = 0;
	}
	if (dx || dy || buttons != touchpad_state.buttons) {
		ps2_mouse_event(pc->mouse, dx, dy, 0, buttons);
		touchpad_state.buttons = buttons;
	}
}

static void poll_keys(PC *pc)
{
	unsigned int index;

	for (index = 0;
	     index < sizeof(keys) / sizeof(keys[0]); index++) {
		bool is_pressed;

		/* Touchpad arrow zones are reported separately by poll_touchpad_mouse. */
		if (is_touchpad &&
		    keys[index].key->tpad_arrow != TPAD_ARROW_NONE)
			continue;
		is_pressed = isKeyPressed(*keys[index].key);
		if (is_pressed == keys[index].is_pressed)
			continue;
		keys[index].is_pressed = is_pressed;
		ps2_put_keycode(pc->kbd, is_pressed,
			       keys[index].keycode);
	}
}

static void configure_defaults(PCConfig *config)
{
	memset(config, 0, sizeof(*config));
	config->mem_size = 16 * 1024 * 1024;
	config->vga_mem_size = 256 * 1024;
	config->width = SCREEN_WIDTH;
	config->height = SCREEN_HEIGHT;
	config->cpu_gen = 4;
	config->fpu = 0;
	config->clock_hz = CPU_HZ;
	config->vga_force_8dm = 1;
}

static void free_config_paths(PCConfig *config)
{
	unsigned int index;

	free((void *)config->linuxstart);
	free((void *)config->kernel);
	free((void *)config->initrd);
	free((void *)config->cmdline);
	free((void *)config->bios);
	free((void *)config->vga_bios);
	free((void *)config->wifi_ssid);
	/* Wiped, not just freed: this is the uplink password. */
	if (config->wifi_password)
		memset((void *)config->wifi_password, 0, strlen(config->wifi_password));
	free((void *)config->wifi_password);
	for (index = 0;
	     index < sizeof(config->disks) / sizeof(config->disks[0]); index++) {
		free((void *)config->disks[index]);
	}
	for (index = 0;
	     index < sizeof(config->fdd) / sizeof(config->fdd[0]); index++) {
		free((void *)config->fdd[index]);
	}
}

/* ------------------------------------------------------------------ */
/* Diagnostic trace sink (DEBUG profile; see cx_profiles.h)             */
/* ------------------------------------------------------------------ */
/*
 * i386.c calls nspire_log() for its diagnostic trace, and only in the DEBUG
 * profile: RELEASE and TURBO compile those call sites out entirely. The
 * calculator has no console, so every line goes to stderr (visible when the
 * program is launched from the Ndless shell) and to winspire.log.tns in the
 * program's folder.
 *
 * The file is flushed per line on purpose. The DEBUG profile exists to
 * diagnose a hang, and a hang must not cost the end of the trace. The trace is
 * exception-driven and low-volume (see the call sites in i386.c), so a flush
 * per line is affordable here; it would not be for per-instruction logging.
 */
static FILE *diag_log_file;

void nspire_log(const char *fmt, ...)
{
	va_list args;

	fputs("[diag] ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	if (!diag_log_file)
		diag_log_file = fopen("winspire.log.tns", "wb");
	if (diag_log_file) {
		va_start(args, fmt);
		vfprintf(diag_log_file, fmt, args);
		va_end(args);
		/* A hang must not cost the tail of the trace. */
		fflush(diag_log_file);
	}
}

static void diag_close_log(void)
{
	if (diag_log_file) {
		fclose(diag_log_file);
		diag_log_file = NULL;
	}
}

static int startup_error(PCConfig *config, const char *message)
{
	free_config_paths(config);
	free_reserved_memory();
	refresh_osscr();
	show_msgbox("WiNspire", message);
	return 1;
}

static void reset_input_state(void)
{
	unsigned int index;

	for (index = 0;
	     index < sizeof(keys) / sizeof(keys[0]); index++)
		keys[index].is_pressed = false;
	memset(&touchpad_state, 0, sizeof(touchpad_state));
}

int main(int argc, char **argv)
{
	const char *config_path = argc > 1 ? argv[1] : "winspire.ini.tns";
	PCConfig config;
	Display display;
	PC *pc;
	uint32_t loops = 0;
	bool first_step_done = false;
	uint32_t saved_cursor = 0;
	int error;
	uint32_t saved_cpu_speed = 0;
	bool cpu_speed_changed = false;

	assert_ndless_rev(2004);
	enable_relative_paths(argv);
	free_reserved_memory();
	reset_input_state();
	mode_changed = false;
	memset(&hw, 0, sizeof(hw));
	boot_error[0] = '\0';
#if defined(WINSPIRE_PROFILE_DEBUG)
	/* First line of the trace: "did this .tns start at all". */
	nspire_log("winspire: %s profile starting\n", WINSPIRE_PROFILE_NAME);
#endif
	if (!detect_hardware()) {
		refresh_osscr();
		show_msgbox("WiNspire", boot_error);
		return 1;
	}
	/*
	 * The original CX clocks its ARM926 at roughly a third of the CX II rate,
	 * so available guest throughput is the binding constraint. Ndless exposes
	 * the clock controller on that hardware (and returns 0 on the CX II, where
	 * the PMU is programmed instead). Ask for the fastest supported step and
	 * restore the previous value on exit; leaving the core overclocked after
	 * returning to TI-OS is not acceptable.
	 */
	if (hw.can_set_cpu_speed) {
		unsigned previous = set_cpu_speed(CPU_SPEED_150MHZ);

		saved_cpu_speed = previous;
		cpu_speed_changed = previous != 0 &&
			previous != CPU_SPEED_150MHZ;
	}

	configure_defaults(&config);
	input_poll_loops = INPUT_POLL_LOOPS;
	video_poll_loops = VIDEO_POLL_LOOPS;
	error = ini_parse(config_path, parse_native_config, &config);
	if (error) {
		if (error > 0)
			snprintf(boot_error, sizeof(boot_error),
				 "Invalid INI entry at line %d:\n%s", error, config_path);
		else if (error == -1)
			snprintf(boot_error, sizeof(boot_error),
				 "Cannot open config:\n%s\nCheck the path and read permissions.",
				 config_path);
		else
			snprintf(boot_error, sizeof(boot_error),
				 "Not enough free RAM to read config:\n%s", config_path);
		startup_error(&config, boot_error);
		return error;
	}
	config.width = SCREEN_WIDTH;
	config.height = SCREEN_HEIGHT;
	config.enable_serial = 0;
	config.vga_force_8dm = 1;
	if (config.clock_hz < CPU_HZ_MIN || config.clock_hz > CPU_HZ_MAX) {
		snprintf(boot_error, sizeof(boot_error),
			 "clock_hz in [cpu] must be between %u and %u in %s.",
			 CPU_HZ_MIN, CPU_HZ_MAX, config_path);
		return startup_error(&config, boot_error);
	}
	if (!is_power_of_two(input_poll_loops) ||
	    !is_power_of_two(video_poll_loops) ||
	    input_poll_loops > POLL_MAX ||
	    video_poll_loops > POLL_MAX) {
		snprintf(boot_error, sizeof(boot_error),
			 "input_poll_loops and video_poll_loops in [nspire] must be "
			 "powers of two from 1 to %u.\n%s", POLL_MAX, config_path);
		return startup_error(&config, boot_error);
	}
	guest_hz = config.clock_hz;
	if (config.mem_size < GUEST_RAM_MIN || config.mem_size > GUEST_RAM_MAX) {
		snprintf(boot_error, sizeof(boot_error),
			 "mem_size must be between %ldM and %ldM in %s.",
			 GUEST_RAM_MIN / (1024L * 1024), GUEST_RAM_MAX / (1024L * 1024),
			 config_path);
		return startup_error(&config, boot_error);
	}
	if (config.vga_mem_size < VGA_RAM_MIN || config.vga_mem_size > VGA_RAM_MAX) {
		snprintf(boot_error, sizeof(boot_error),
			 "vga_mem_size must be between %ldK and %ldK in %s.",
			 VGA_RAM_MIN / 1024L, VGA_RAM_MAX / 1024L, config_path);
		return startup_error(&config, boot_error);
	}
	/*
	 * Check the uplink credentials before anything is allocated, so a typo is a
	 * message box and not a silent failure 40 seconds into Windows 95. The
	 * same validator runs in the bridge firmware, so passing here means the
	 * bridge will accept the pair.
	 */
	if (config.wifi_ssid || config.wifi_password) {
		CxlinkWifiResult uplink = cxlink_wifi_check(config.wifi_ssid,
							   config.wifi_password);

		if (uplink != CXLINK_WIFI_OK) {
			snprintf(boot_error, sizeof(boot_error),
				 "[network] in %s: %s.\n"
				 "ssid is 1-31 characters, password 0 (open "
				 "network) or 8-63.",
				 config_path, cxlink_wifi_result_text(uplink));
			return startup_error(&config, boot_error);
		}
		/* Empty ssid with a password is refused above, so this is "asked
		 * for a network". */
		uplink_configured = config.wifi_ssid && config.wifi_ssid[0];
	}
	if (!preflight_boot_files(&config))
		return startup_error(&config, boot_error);
	if (!reserve_guest_memory(&config)) {
		snprintf(boot_error, sizeof(boot_error),
			 "Could not allocate %ld KiB of guest RAM.\n"
			 "Lower mem_size in %s.", config.mem_size / 1024L, config_path);
		return startup_error(&config, boot_error);
	}
	if (!reserve_vga_memory(&config)) {
		snprintf(boot_error, sizeof(boot_error),
			 "Could not allocate %ld KiB of video memory.\n"
			 "Lower mem_size or vga_mem_size in %s.",
			 config.vga_mem_size / 1024L, config_path);
		return startup_error(&config, boot_error);
	}

	memset(&display, 0, sizeof(display));
	display.framebuffer = calloc(1, FRAMEBUFFER_BYTES);
	if (!display.framebuffer) {
		snprintf(boot_error, sizeof(boot_error),
			 "Not enough free RAM for the calculator display.\n"
			 "Lower mem_size in %s.", config_path);
		return startup_error(&config, boot_error);
	}
	if (hw.rotated_panel) {
		display.rotate_buffer = calloc(1, ROTATE_BUFFER_BYTES);
		if (!display.rotate_buffer) {
			snprintf(boot_error, sizeof(boot_error),
				 "Not enough free RAM for the rotating panel buffer.\n"
				 "Lower mem_size in %s.", config_path);
			free(display.framebuffer);
			display.framebuffer = NULL;
			return startup_error(&config, boot_error);
		}
	} else {
		/*
		 * The panel presents the surface rotated 180 degrees, so
		 * every present needs a corrected copy.
		 * See rotate_surface_180().
		 */
		display.flip_buffer = calloc(1, ROTATE_BUFFER_BYTES);
		if (!display.flip_buffer) {
			snprintf(boot_error, sizeof(boot_error),
				 "Not enough free RAM for the panel rotation buffer.\n"
				 "Lower mem_size in %s.", config_path);
			free(display.framebuffer);
			display.framebuffer = NULL;
			return startup_error(&config, boot_error);
		}
	}
	reset_guest_timer();
	pc = pc_new(redraw, &display, display.framebuffer, &config);
	load_bios_and_reset(pc);
	saved_cursor = hide_os_cursor();
	vga_refresh(pc->vga, redraw, &display, 1);
	reset_guest_timer();
	i8254_rebase(pc->pit);
	pc->boot_start_time = get_uticks();
	io_bridge_reset();
	bridge_config = &config;
	cxlink_start_default();

	while (pc->shutdown_state != 8 && !on_key_pressed()) {
		pc_step(pc);
		advance_guest_timer(pc);
		service_io_bridge(pc);
		if (!first_step_done) {
			if (display.ready) {
				draw_frame(&display, true);
			}
			first_step_done = true;
		}
		loops++;
		if ((loops & (input_poll_loops - 1)) == 0) {
			poll_keys(pc);
			poll_touchpad_mouse(pc);
		}
		if ((loops & (video_poll_loops - 1)) == 0) {
			pc_vga_step(pc);
			flush_redraw(&display);
			if (display.ready)
				keep_lcd(&display);
		}
	}
	if (on_key_pressed())
		wait_no_key_pressed();
	/*
	 * Drain the held partial audio block and tell the ESP32 to play out what it
	 * has, before the UART goes away with the emulator.
	 */
	audio_samples_sent += cxlink_audio_resampler_feed(&audio_resampler, NULL, 0,
							  true);
	cxlink_audio_flush();
	/*
	 * One-shot bridge summary, and only when there is something to report:
	 * audio that never reached a link, or audio the scheduler had to drop.
	 * The healthy case stays silent, so this cannot turn into exit noise.
	 */
	if (audio_frames_dropped || (audio_samples_sent && !cxlink_link_up())) {
		char bridge_note[192];

		snprintf(bridge_note, sizeof(bridge_note),
			 "Audio bridge: %u PCM blocks sent, %u dropped.\n"
			 "%s\nSee CX_PORT_STATUS.md for the ESP32 link setup.",
			 (unsigned)audio_samples_sent,
			 (unsigned)audio_frames_dropped,
			 cxlink_link_up() ? "ESP32 link was up."
					  : "No ESP32 link came up.");
		show_msgbox("WiNspire", bridge_note);
	}
	/*
	 * Uplink summary, again only when there is something to report: a Wi-Fi
	 * password that was refused at startup, credentials that never got a
	 * confirmation, or a bridge that answered refused to associate. The
	 * healthy case (confirmed, connected) stays silent, exactly like the audio
	 * note above. The password never appears in any of this.
	 */
	if (provision_note_valid || cxlink_link_up() || uplink_configured) {
		CxlinkProvisionStatus prov;
		const char *uplink_text = NULL;

		cxlink_net_provision_status(&prov);
		if (uplink_configured && !provision_done)
			/* The most common failure with no hardware attached, and
			 * otherwise completely silent. */
			uplink_text = "No ESP32 answered on the dock link, so the "
				      "Wi-Fi credentials were never sent.\n"
				      "Check the dock connection and "
				      "-DWINSPIRE_CXLINK_UART.";
		else if (prov.state == CXLINK_NET_STATE_REJECTED)
			uplink_text = "The access point refused the credentials.";
		else if (prov.state == CXLINK_NET_STATE_CONNECTED)
			uplink_text = NULL; /* healthy: say nothing */
		else if (prov.confirmed)
			uplink_text = "The ESP32 has the credentials but is not "
				      "associated yet.";
		else if (prov.pending)
			uplink_text = "The ESP32 never confirmed the credentials "
				      "(send count below).";
		else if (provision_note_valid)
			uplink_text = provision_note;

		if (uplink_text) {
			char uplink_note[192];

			snprintf(uplink_note, sizeof(uplink_note),
				 "%s\nUplink: %u credential frame(s) sent, "
				 "%s.\nSee NETWORK_ARCHITECTURE.md 6.3.",
				 uplink_text, (unsigned)prov.sends,
				 prov.confirmed ? "confirmed by the ESP32"
						: "never confirmed");
			show_msgbox("WiNspire", uplink_note);
		}
	}
	/*
	 * Forget the uplink password on the way out. It is not needed once the
	 * bridge holds it, and a secret should not stay in RAM after the emulator
	 * has exited. (The bridge keeps its own RAM-only copy; the next session
	 * provisions it again automatically.)
	 */
	cxlink_net_provision_clear();
	if (config.wifi_password)
		memset((void *)config.wifi_password, 0, strlen(config.wifi_password));
	/* No-op outside the DEBUG profile, which writes winspire.log.tns. */
	diag_close_log();
	lcd_init(SCR_TYPE_INVALID);
	restore_os_cursor(saved_cursor);
	/* Never leave the ARM926 overclocked after handing control back to TI-OS. */
	if (cpu_speed_changed)
		set_cpu_speed(saved_cpu_speed);
	refresh_osscr();
	pc_free_buffers(pc);
	free(display.rotate_buffer);
	free(display.flip_buffer);
	free(display.framebuffer);
	free_config_paths(&config);
	free_reserved_memory();
	return 0;
}
