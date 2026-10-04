/*
 * Shared key map.
 *
 * This lives in a header so the calculator frontend and the host input self
 * test compile against ONE definition. A transcribed copy is worse than no
 * test: it keeps passing when the thing it is meant to check drifts.
 *
 * Requires <libndls.h> (or the Ndless key stub) to have been included first,
 * for KEY_NSPIRE_* and t_key.
 *
 * The array is static, so it has internal linkage and is safe in any number of
 * translation units.
 */

#ifndef WINSPIRE_NDLESS_KEYMAP_H
#define WINSPIRE_NDLESS_KEYMAP_H

/* PC set-1 scancodes. F13-F15 are E0-prefixed; ps2_put_keycode() takes them
 * as 0xe0xx. */
#define SCAN_F1  0x3b
#define SCAN_F2  0x3c
#define SCAN_F3  0x3d
#define SCAN_F4  0x3e
#define SCAN_F5  0x3f
#define SCAN_F6  0x40
#define SCAN_F7  0x41
#define SCAN_F8  0x42
#define SCAN_F9  0x43
#define SCAN_F10 0x44
#define SCAN_F11 0x57
#define SCAN_F12 0x58
#define SCAN_F13 0xe068
#define SCAN_F14 0xe069
#define SCAN_F15 0xe06a

#define KEY_SEMICOLON 0x27
#define KEY_APOSTROPHE 0x34
#define KEY_ASTERISK 0x33
#define KEY_ALT 58

/*
 * A chord rather than an on-screen keyboard, deliberately: the panel is
 * 320x240 and is already the guest's entire screen, so a keyboard overlay
 * would cover the thing the user is trying to operate, and a chord costs no
 * screen space at all.
 *
 * The CX has no F1-F15 keys, so holding Ctrl+Menu(Alt) turns the number row
 * and the three punctuation keys below it into the fifteen function keys,
 * left to right:
 *
 *     1 2 3 4 5 6 7 8 9 0 - =   ->  F1 .. F12
 *     , . /                      ->  F13 F14 F15
 *
 * Every one of those keys keeps its ordinary meaning whenever the chord is not
 * held, so nothing is given up to get F1-F15.
 *
 * fn_consumed records that the Fn layer has claimed a key, so that releasing
 * the chord first does not then type the underlying character.
 */
typedef struct {
	const t_key *key;
	int keycode;
	bool is_pressed;
	int fn_keycode;
	bool fn_pressed;
	bool fn_consumed;
} KeyBinding;

static KeyBinding keymap[] = {
	{ &KEY_NSPIRE_ESC, 1, false, 0, false, false },
	{ &KEY_NSPIRE_1, 2, false, SCAN_F1, false, false },
	{ &KEY_NSPIRE_2, 3, false, SCAN_F2, false, false },
	{ &KEY_NSPIRE_3, 4, false, SCAN_F3, false, false },
	{ &KEY_NSPIRE_4, 5, false, SCAN_F4, false, false },
	{ &KEY_NSPIRE_5, 6, false, SCAN_F5, false, false },
	{ &KEY_NSPIRE_6, 7, false, SCAN_F6, false, false },
	{ &KEY_NSPIRE_7, 8, false, SCAN_F7, false, false },
	{ &KEY_NSPIRE_8, 9, false, SCAN_F8, false, false },
	{ &KEY_NSPIRE_9, 10, false, SCAN_F9, false, false },
	{ &KEY_NSPIRE_0, 11, false, SCAN_F10, false, false },
	{ &KEY_NSPIRE_MINUS, 12, false, SCAN_F11, false, false },
	{ &KEY_NSPIRE_EQU, 13, false, SCAN_F12, false, false },
	{ &KEY_NSPIRE_DEL, 14, false, 0, false, false },
	{ &KEY_NSPIRE_TAB, 15, false, 0, false, false },
	{ &KEY_NSPIRE_Q, 16, false, 0, false, false },
	{ &KEY_NSPIRE_W, 17, false, 0, false, false },
	{ &KEY_NSPIRE_E, 18, false, 0, false, false },
	{ &KEY_NSPIRE_R, 19, false, 0, false, false },
	{ &KEY_NSPIRE_T, 20, false, 0, false, false },
	{ &KEY_NSPIRE_Y, 21, false, 0, false, false },
	{ &KEY_NSPIRE_U, 22, false, 0, false, false },
	{ &KEY_NSPIRE_I, 23, false, 0, false, false },
	{ &KEY_NSPIRE_O, 24, false, 0, false, false },
	{ &KEY_NSPIRE_P, 25, false, 0, false, false },
	{ &KEY_NSPIRE_ENTER, 28, false, 0, false, false },
	{ &KEY_NSPIRE_CTRL, 29, false, 0, false, false },
	{ &KEY_NSPIRE_A, 30, false, 0, false, false },
	{ &KEY_NSPIRE_S, 31, false, 0, false, false },
	{ &KEY_NSPIRE_D, 32, false, 0, false, false },
	{ &KEY_NSPIRE_F, 33, false, 0, false, false },
	{ &KEY_NSPIRE_G, 34, false, 0, false, false },
	{ &KEY_NSPIRE_H, 35, false, 0, false, false },
	{ &KEY_NSPIRE_J, 36, false, 0, false, false },
	{ &KEY_NSPIRE_K, 37, false, 0, false, false },
	{ &KEY_NSPIRE_L, 38, false, 0, false, false },
	{ &KEY_NSPIRE_SHIFT, 42, false, 0, false, false },
	{ &KEY_NSPIRE_Z, 44, false, 0, false, false },
	{ &KEY_NSPIRE_X, 45, false, 0, false, false },
	{ &KEY_NSPIRE_C, 46, false, 0, false, false },
	{ &KEY_NSPIRE_V, 47, false, 0, false, false },
	{ &KEY_NSPIRE_B, 48, false, 0, false, false },
	{ &KEY_NSPIRE_N, 49, false, 0, false, false },
	{ &KEY_NSPIRE_M, 50, false, 0, false, false },
	{ &KEY_NSPIRE_COMMA, 51, false, SCAN_F13, false, false },
	{ &KEY_NSPIRE_PERIOD, 52, false, SCAN_F14, false, false },
	{ &KEY_NSPIRE_DIVIDE, 53, false, SCAN_F15, false, false },
	{ &KEY_NSPIRE_VAR, KEY_SEMICOLON, false, 0, false, false },
	{ &KEY_NSPIRE_MULTIPLY, KEY_ASTERISK, false, 0, false, false },
	{ &KEY_NSPIRE_APOSTROPHE, KEY_APOSTROPHE, false, 0, false, false },
	{ &KEY_NSPIRE_SPACE, 57, false, 0, false, false },
	/*
	 * The CX has no Alt key at all, and Windows 95 needs one for every
	 * menu mnemonic and for Alt+Tab, so the hardware "menu" key - which
	 * has no other use in a PC guest - stands in for it. It is also
	 * half of the Fn chord.
	 */
	{ &KEY_NSPIRE_MENU, KEY_ALT, false, 0, false, false },
	{ &KEY_NSPIRE_HOME, 102, false, 0, false, false },
	{ &KEY_NSPIRE_UP, 103, false, 0, false, false },
	{ &KEY_NSPIRE_LEFT, 105, false, 0, false, false },
	{ &KEY_NSPIRE_RIGHT, 106, false, 0, false, false },
	{ &KEY_NSPIRE_DOWN, 108, false, 0, false, false },
};

#define WINSPIRE_KEYMAP_COUNT \
	(sizeof(keymap) / sizeof(keymap[0]))

#endif /* WINSPIRE_NDLESS_KEYMAP_H */