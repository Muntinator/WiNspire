/*
 * Typecheck-only stub of the Ndless <keys.h>.
 *
 * The shapes here mirror the real header in the Ndless SDK
 * (ndless-sdk/include/keys.h): t_key carries both keypad and touchpad
 * coordinates plus the touchpad arrow zone. The numeric row/column values are
 * placeholders - they are irrelevant to compiling the frontend, and this stub
 * is never used to build anything that runs on a calculator.
 */
#ifndef WINSPIRE_NDLESS_STUB_KEYS_H
#define WINSPIRE_NDLESS_STUB_KEYS_H

typedef enum tpad_arrow {
	TPAD_ARROW_NONE,
	TPAD_ARROW_UP, TPAD_ARROW_UPRIGHT,
	TPAD_ARROW_RIGHT, TPAD_ARROW_RIGHTDOWN,
	TPAD_ARROW_DOWN, TPAD_ARROW_DOWNLEFT,
	TPAD_ARROW_LEFT, TPAD_ARROW_LEFTUP,
	TPAD_ARROW_CLICK
} tpad_arrow_t;

typedef struct {
	int row, col, tpad_row, tpad_col;
	tpad_arrow_t tpad_arrow;
} t_key;

#define KEY_(row, col) {row, col, row, col, TPAD_ARROW_NONE}
#define KEYTPAD_(row, col, tpad_row, tpad_col) {row, col, tpad_row, tpad_col, TPAD_ARROW_NONE}
#define KEYTPAD_ARROW_(row, col, tpad_arrow) {row, col, row, col, tpad_arrow}

static const t_key KEY_NSPIRE_ESC = KEY_(0x00, 0x001);
static const t_key KEY_NSPIRE_TAB = KEY_(0x00, 0x002);
static const t_key KEY_NSPIRE_SHIFT = KEY_(0x00, 0x004);
static const t_key KEY_NSPIRE_CTRL = KEY_(0x00, 0x008);
static const t_key KEY_NSPIRE_HOME = KEY_(0x00, 0x010);
static const t_key KEY_NSPIRE_UP = KEYTPAD_ARROW_(0x00, 0x020, TPAD_ARROW_UP);
static const t_key KEY_NSPIRE_LEFT = KEYTPAD_ARROW_(0x00, 0x040, TPAD_ARROW_LEFT);
static const t_key KEY_NSPIRE_RIGHT = KEYTPAD_ARROW_(0x00, 0x080, TPAD_ARROW_RIGHT);
static const t_key KEY_NSPIRE_DOWN = KEYTPAD_ARROW_(0x00, 0x100, TPAD_ARROW_DOWN);

static const t_key KEY_NSPIRE_RET = KEY_(0x10, 0x001);
static const t_key KEY_NSPIRE_ENTER = KEY_(0x10, 0x002);
static const t_key KEY_NSPIRE_SPACE = KEYTPAD_(0x10, 0x004, 0x10, 0x10);
static const t_key KEY_NSPIRE_NEGATIVE = KEY_(0x10, 0x008);
static const t_key KEY_NSPIRE_Z = KEYTPAD_(0x10, 0x010, 0x10, 0x20);
static const t_key KEY_NSPIRE_PERIOD = KEYTPAD_(0x10, 0x020, 0x1A, 0x010);
static const t_key KEY_NSPIRE_Y = KEY_(0x10, 0x040);
static const t_key KEY_NSPIRE_0 = KEY_(0x10, 0x080);
static const t_key KEY_NSPIRE_X = KEYTPAD_(0x10, 0x100, 0x12, 0x001);
static const t_key KEY_NSPIRE_W = KEYTPAD_(0x10, 0x200, 0x14, 0x001);
static const t_key KEY_NSPIRE_V = KEYTPAD_(0x10, 0x400, 0x12, 0x004);

static const t_key KEY_NSPIRE_Q = KEYTPAD_(0x11, 0x001, 0x14, 0x010);
static const t_key KEY_NSPIRE_U = KEYTPAD_(0x11, 0x002, 0x1A, 0x008);
static const t_key KEY_NSPIRE_E = KEYTPAD_(0x11, 0x004, 0x14, 0x020);
static const t_key KEY_NSPIRE_O = KEY_(0x11, 0x008);
static const t_key KEY_NSPIRE_T = KEYTPAD_(0x11, 0x010, 0x1A, 0x020);
static const t_key KEY_NSPIRE_DIVIDE = KEY_(0x11, 0x020);
static const t_key KEY_NSPIRE_9 = KEY_(0x11, 0x040);
static const t_key KEY_NSPIRE_8 = KEY_(0x11, 0x080);
static const t_key KEY_NSPIRE_7 = KEY_(0x11, 0x100);
static const t_key KEY_NSPIRE_6 = KEY_(0x11, 0x200);
static const t_key KEY_NSPIRE_5 = KEY_(0x11, 0x400);

static const t_key KEY_NSPIRE_A = KEYTPAD_(0x12, 0x001, 0x16, 0x008);
static const t_key KEY_NSPIRE_J = KEY_(0x12, 0x002);
static const t_key KEY_NSPIRE_D = KEYTPAD_(0x12, 0x004, 0x18, 0x008);
static const t_key KEY_NSPIRE_M = KEY_(0x12, 0x008);
static const t_key KEY_NSPIRE_VAR = KEY_(0x12, 0x010);
static const t_key KEY_NSPIRE_L = KEY_(0x12, 0x020);
static const t_key KEY_NSPIRE_4 = KEY_(0x12, 0x040);
static const t_key KEY_NSPIRE_3 = KEY_(0x12, 0x080);
static const t_key KEY_NSPIRE_2 = KEY_(0x12, 0x100);
static const t_key KEY_NSPIRE_1 = KEY_(0x12, 0x200);
static const t_key KEY_NSPIRE_EQU = KEY_(0x12, 0x400);

static const t_key KEY_NSPIRE_S = KEYTPAD_(0x13, 0x001, 0x18, 0x004);
static const t_key KEY_NSPIRE_I = KEYTPAD_(0x13, 0x002, 0x1A, 0x004);
static const t_key KEY_NSPIRE_F = KEYTPAD_(0x13, 0x004, 0x16, 0x004);
static const t_key KEY_NSPIRE_K = KEY_(0x13, 0x008);
static const t_key KEY_NSPIRE_MINUS = KEY_(0x13, 0x010);
static const t_key KEY_NSPIRE_G = KEY_(0x13, 0x020);
static const t_key KEY_NSPIRE_H = KEY_(0x13, 0x040);
static const t_key KEY_NSPIRE_B = KEYTPAD_(0x13, 0x080, 0x12, 0x020);
static const t_key KEY_NSPIRE_N = KEY_(0x13, 0x100);
static const t_key KEY_NSPIRE_C = KEYTPAD_(0x13, 0x200, 0x16, 0x020);
static const t_key KEY_NSPIRE_COMMA = KEY_(0x13, 0x400);

static const t_key KEY_NSPIRE_R = KEYTPAD_(0x14, 0x001, 0x14, 0x040);
static const t_key KEY_NSPIRE_P = KEY_(0x14, 0x002);
static const t_key KEY_NSPIRE_DEL = KEY_(0x14, 0x004);

/* Values copied verbatim from the Ndless SDK's keys.h (ndless-sdk/include). */
static const t_key KEY_NSPIRE_MULTIPLY = KEYTPAD_(0x16, 0x002, 0x18, 0x100);
static const t_key KEY_NSPIRE_APOSTROPHE = KEY_(0x1A, 0x001);
static const t_key KEY_NSPIRE_MENU = KEY_(0x1C, 0x020);
static const t_key KEY_NSPIRE_CLICK =
	KEYTPAD_ARROW_(0x1C, 0x002, TPAD_ARROW_CLICK);

#endif /* WINSPIRE_NDLESS_STUB_KEYS_H */
