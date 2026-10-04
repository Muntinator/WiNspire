/*
 * Input self test for the calculator frontend's key handling.
 *
 * Drives the REAL PS/2 code (i8042.c) through the routing transcribed from
 * source/winspire-ndless/main.c and accounts for every byte a guest would read
 * from port 0x60. Nothing is asserted loosely: each step expects an exact
 * byte sequence, and the queue must be empty between steps.
 *
 * Covers:
 *   - Fn + 1..0 - =  produces F1..F12 (F13..F15 from , . / are E0-prefixed)
 *   - every function key also produces its break code
 *   - without the chord those same keys keep their ordinary meaning
 *   - Ctrl alone is still Ctrl, not Fn
 *   - nothing sticks, and no key re-fires when the chord is dropped
 *
 * Build (from the repo root):
 *   cc -O1 -w -I source/winspire -o /tmp/input_selftest \
 *      source/host/input_selftest.c source/winspire/i8042.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "i8042.h"
#include "input_selftest.h"

static int failures;
static int checks;
int input_selftest_failure_line;

static KBDState *g_kbd;
static PS2KbdState *g_ps2kbd;

/*
 * get_uticks() is defined once by the host frontend (source/host/main.c) and
 * drives i8042.c's E0 second-byte delay through the real monotonic clock, so
 * the delayed F13-F15 codes are exercised exactly as they are on hardware.
 */

/*
 * The key map is the SAME header the calculator frontend compiles against, so
 * this cannot drift away from what ships: a transcribed copy would keep
 * passing while the real table changed underneath it.
 */
#include <keys.h>
#include <keymap.h>

static bool ctrl_held;
static bool menu_held;
static bool fn_mode;
static const t_key *held_key;

static bool fn_mode_active(void)
{
	return ctrl_held && menu_held;
}

static void reset_keymap_state(void)
{
	unsigned int i;

	for (i = 0; i < WINSPIRE_KEYMAP_COUNT; i++) {
		keymap[i].is_pressed = false;
		keymap[i].fn_pressed = false;
		keymap[i].fn_consumed = false;
	}
}

/* Held physical key, identified by its t_key rather than a local enum. */
static const t_key *held_key;

static void poll_keys(void)
{
	unsigned int i;
	bool fn = fn_mode_active();

	if (fn != fn_mode) {
		for (i = 0; i < WINSPIRE_KEYMAP_COUNT; i++) {
			bool *held = fn ? &keymap[i].is_pressed
					: &keymap[i].fn_pressed;
			int code = fn ? keymap[i].keycode : keymap[i].fn_keycode;

			if (*held && code)
				ps2_put_keycode(g_ps2kbd, 0, code);
			*held = false;
		}
		fn_mode = fn;
	}
	for (i = 0; i < WINSPIRE_KEYMAP_COUNT; i++) {
		bool pressed = (held_key == keymap[i].key);
		bool *held;
		int code;

		/* Touchpad arrow zones are handled by the touchpad path. */
		if (keymap[i].key->tpad_arrow != TPAD_ARROW_NONE)
			continue;
		if (fn) {
			if (keymap[i].fn_keycode == 0)
				continue;
			code = keymap[i].fn_keycode;
			held = &keymap[i].fn_pressed;
		} else {
			code = keymap[i].keycode;
			held = &keymap[i].is_pressed;
		}
		if (fn) {
			if (pressed && !*held)
				keymap[i].fn_consumed = true;
		} else if (keymap[i].fn_consumed) {
			if (pressed)
				continue;
			keymap[i].fn_consumed = false;
			continue;
		}
		if (pressed == *held)
			continue;
		*held = pressed;
		ps2_put_keycode(g_ps2kbd, pressed, code);
	}
}

/* --- test plumbing --- */

static void set_irq(void *pic, int irq, int level)
{
	(void)pic;
	(void)irq;
	(void)level;
}

static void pump(void)
{
	int guard;

	for (guard = 0; guard < 400; guard++) {
		struct timespec ts = { 0, 1000000 };

		kbd_step(g_kbd);
		if (!(kbd_read_status(g_kbd, 0) & 0x01))
			break;
		nanosleep(&ts, NULL);
	}
	kbd_step(g_kbd);
}

static int drain(int *out)
{
	int count = 0;

	while (kbd_read_status(g_kbd, 0) & 0x01) {
		if (count < 32)
			out[count] = (int)(kbd_read_data(g_kbd, 0) & 0xff);
		count++;
		pump();
	}
	return count;
}

/* Assert the guest reads exactly these bytes, and nothing else. */
static void expect(const char *what, const int *want, int want_count)
{
	int got[32];
	int n;
	int i;

	checks++;
	n = drain(got);
	if (n != want_count || (want_count &&
	    memcmp(got, want, sizeof(int) * want_count) != 0)) {
		input_selftest_failure_line = __LINE__;
		printf("  FAIL %-34s want", what);
		for (i = 0; i < want_count; i++)
			printf(" %02x", want[i]);
		printf("  got");
		for (i = 0; i < n && i < 8; i++)
			printf(" %02x", got[i]);
		printf(" (%d byte(s))\n", n);
		failures++;
	}
}

static void expect_empty(const char *what)
{
	int got[32];

	expect(what, NULL, 0);
	(void)got;
}

static void reset_input(void)
{
	ctrl_held = false;
	menu_held = false;
	held_key = NULL;
	reset_keymap_state();
	fn_mode = false;
}

static void quiesce(void)
{
	/* Bring every key up with the chord held, then drop the chord. */
	held_key = NULL;
	ctrl_held = true;
	menu_held = true;
	poll_keys();
	pump();
	ctrl_held = false;
	menu_held = false;
	poll_keys();
	pump();
	reset_input();
	drain(NULL);
}

static int run_all(void)
{
	static const struct {
		const char *name;
		const t_key *key;
		int make;
		bool e0;
	} fkeys[] = {
		{ "F1",  &KEY_NSPIRE_1,      SCAN_F1,  false },
		{ "F2",  &KEY_NSPIRE_2,      SCAN_F2,  false },
		{ "F3",  &KEY_NSPIRE_3,      SCAN_F3,  false },
		{ "F4",  &KEY_NSPIRE_4,      SCAN_F4,  false },
		{ "F5",  &KEY_NSPIRE_5,      SCAN_F5,  false },
		{ "F6",  &KEY_NSPIRE_6,      SCAN_F6,  false },
		{ "F7",  &KEY_NSPIRE_7,      SCAN_F7,  false },
		{ "F8",  &KEY_NSPIRE_8,      SCAN_F8,  false },
		{ "F9",  &KEY_NSPIRE_9,      SCAN_F9,  false },
		{ "F10", &KEY_NSPIRE_0,      SCAN_F10, false },
		{ "F11", &KEY_NSPIRE_MINUS,  SCAN_F11, false },
		{ "F12", &KEY_NSPIRE_EQU,    SCAN_F12, false },
		{ "F13", &KEY_NSPIRE_COMMA,  0x68, true },
		{ "F14", &KEY_NSPIRE_PERIOD, 0x69, true },
		{ "F15", &KEY_NSPIRE_DIVIDE, 0x6a, true },
	};
	PS2MouseState *mouse;
	unsigned int i;
	int before;

	g_kbd = i8042_init(&g_ps2kbd, &mouse, 1, 12, NULL, set_irq, NULL, NULL);
	if (!g_kbd) {
		printf("i8042_init failed\n");
		input_selftest_failure_line = __LINE__;
		return 1;
	}
	printf("input self test: calculator key handling through PS/2\n\n");

	printf("Fn chord + a key gives a function key, press and release:\n");
	for (i = 0; i < sizeof(fkeys) / sizeof(fkeys[0]); i++) {
		int want[2];
		int n = fkeys[i].e0 ? 2 : 1;

		quiesce();
		ctrl_held = true;
		menu_held = true;
		held_key = fkeys[i].key;
		poll_keys();
		pump();
		want[0] = fkeys[i].e0 ? 0xe0 : fkeys[i].make;
		if (fkeys[i].e0)
			want[1] = fkeys[i].make;
		expect("fn make", want, n);

		held_key = NULL;
		poll_keys();
		pump();
		want[0] = fkeys[i].e0 ? 0xe0 : (fkeys[i].make | 0x80);
		if (fkeys[i].e0)
			want[1] = fkeys[i].make | 0x80;
		expect("fn break", want, n);
		printf("  %-4s -> 0x%02x%s  %s\n", fkeys[i].name,
		       fkeys[i].make, fkeys[i].e0 ? " +E0" : "",
		       failures ? "FAIL" : "ok");
	}

	printf("\nwithout the chord those keys keep their meaning:\n");
	before = failures;
	quiesce();
	{
		int want[1];

		held_key = &KEY_NSPIRE_1;
		poll_keys();
		pump();
		want[0] = 0x02;               /* set-1 make for '1' */
		expect("digit 1 makes", want, 1);
		held_key = NULL;
		poll_keys();
		pump();
		want[0] = 0x82;               /* set-1 break for '1' */
		expect("digit 1 breaks", want, 1);

		held_key = &KEY_NSPIRE_COMMA;
		poll_keys();
		pump();
		want[0] = 51;                 /* set-1 make for ',' */
		expect("comma makes", want, 1);
		held_key = NULL;
		poll_keys();
		pump();
		want[0] = 51 | 0x80;
		expect("comma breaks", want, 1);
	}
	printf("  -> %s\n", failures == before ? "OK" : "FAIL");

	printf("\nCtrl alone is Ctrl, not Fn:\n");
	before = failures;
	quiesce();
	{
		int want[1];

		ctrl_held = true;
		held_key = &KEY_NSPIRE_1;
		poll_keys();
		pump();
		want[0] = 0x02;               /* '1', not F1 */
		expect("ctrl+1 types 1", want, 1);

		held_key = NULL;
		poll_keys();
		pump();
		want[0] = 0x82;
		expect("ctrl+1 releases", want, 1);

		held_key = &KEY_NSPIRE_CTRL;
		poll_keys();
		pump();
		want[0] = 0x1d;
		expect("ctrl makes", want, 1);
		held_key = NULL;
		poll_keys();
		pump();
		want[0] = 0x9d;
		expect("ctrl breaks", want, 1);
		ctrl_held = false;
		poll_keys();
		pump();
	}
	printf("  -> %s\n", failures == before ? "OK" : "FAIL");

	printf("\ndropping the chord mid-press releases cleanly:\n");
	before = failures;
	quiesce();
	{
		int want[1];

		ctrl_held = true;
		menu_held = true;
		held_key = &KEY_NSPIRE_5;
		poll_keys();
		pump();
		want[0] = 0x3f;               /* F5 make */
		expect("F5 makes", want, 1);

		/*
		 * Alt goes up while F5 is still held. The function key must
		 * be released, and the still-held 5 must NOT then be typed.
		 */
		menu_held = false;
		poll_keys();
		pump();
		want[0] = 0xbf;               /* F5 break */
		expect("F5 breaks", want, 1);
		expect_empty("nothing leaks on chord drop");

		held_key = NULL;
		poll_keys();
		pump();
		expect_empty("5 is not retyped");

		ctrl_held = false;
		poll_keys();
		pump();
	}
	printf("  -> %s\n", failures == before ? "OK" : "FAIL");

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	if (failures) {
		input_selftest_failure_line = __LINE__;
		return 1;
	}
	printf("RESULT: PASS (F1-F15 reach the guest; nothing sticks)\n");
	return 0;
}

/* Entry point used by the host frontend's --selftest. */
bool input_selftest(void)
{
	input_selftest_failure_line = 0;
	checks = 0;
	failures = 0;
	/* run_all() is an int main(), so 0 means success. */
	return run_all() == 0;
}
