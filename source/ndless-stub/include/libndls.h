/*
 * Typecheck-only stub of the Ndless <libndls.h>.
 *
 * Purpose: let the CX frontend (source/winspire-ndless/main.c) be compiled on a
 * development host without the Ndless SDK, so the port can be type-checked in
 * CI and in the Freebuff workspace. Run it with:
 *
 *   source/build-scripts/check_cx_frontend.sh
 *
 * This is NOT a substitute for the SDK. Calculator builds use the real header
 * from $NDLESS_SDK/include. Values and signatures below are copied from the
 * real header so that a mismatch between the frontend and the SDK shows up as
 * a compile error here rather than on a calculator.
 *
 * Facts encoded here (verified against the Ndless SDK sources):
 *   - hwtype() < 1 is "classic" grayscale (PL110, 4bpp).
 *   - hwsubtype() == 1 is CM, == 2 is CX II. The original CX is hwtype() >= 1
 *     with hwsubtype() == 0, i.e. neither is_classic nor is_cm nor is_cx2.
 *   - SCR_240x320_565 is the rotated 16bpp panel used by CX revision W and
 *     later, and by the CX II.
 *   - CPU_SPEED_* values feed set_cpu_speed(), which is a no-op on the CX II.
 */
#ifndef WINSPIRE_NDLESS_STUB_LIBNDLS_H
#define WINSPIRE_NDLESS_STUB_LIBNDLS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <keys.h>

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

typedef int BOOL;

typedef struct {
	uint16_t width;
	uint16_t height;
} touchpad_info_t;

typedef struct {
	unsigned char contact;
	unsigned char proximity;
	uint16_t x;
	uint16_t y;
	unsigned char x_velocity;
	unsigned char y_velocity;
	uint16_t dummy;
	unsigned char pressed;
	unsigned char arrow;
} touchpad_report_t;

typedef enum {
	SCR_TYPE_INVALID = -1,
	SCR_320x240_565 = 0,
	SCR_320x240_4 = 1,
	SCR_240x320_565 = 2,
	SCR_320x240_16 = 3,
	SCR_320x240_8 = 4,
	SCR_320x240_555 = 5,
	SCR_240x320_555 = 6,
	SCR_TYPE_COUNT = 7
} scr_type_t;

/* for set_cpu_speed() */
#define CPU_SPEED_150MHZ 0x00000002
#define CPU_SPEED_120MHZ 0x000A1002
#define CPU_SPEED_90MHZ  0x00141002

void assert_ndless_rev(unsigned required_rev);
BOOL any_key_pressed(void);
int enable_relative_paths(char **argv);
BOOL on_key_pressed(void);
void refresh_osscr(void);
unsigned set_cpu_speed(unsigned speed);
BOOL isKeyPressed(const t_key *key);
#define isKeyPressed(x) isKeyPressed(&x)
void wait_no_key_pressed(void);
void wait_key_pressed(void);

bool lcd_init(scr_type_t type);
void lcd_blit(void *buffer, scr_type_t buffer_type);
scr_type_t lcd_type(void);

touchpad_info_t *touchpad_getinfo(void);
int touchpad_scan(touchpad_report_t *report);
BOOL touchpad_arrow_pressed(tpad_arrow_t arrow);
BOOL _is_touchpad(void);
#define is_touchpad _is_touchpad()

unsigned _show_msgbox(const char *title, const char *msg, unsigned button_num, ...);
#define show_msgbox(title, msg) _show_msgbox(title, msg, 0)

unsigned hwtype(void);
#define is_classic (hwtype() < 1)
#define is_cm (nl_hwsubtype() == 1)
#define is_cx2 (nl_hwsubtype() == 2)
#define has_colors (!is_classic)

unsigned nl_hwsubtype(void);

#define IO_LCD_CONTROL IO(0xC000001C, 0xC0000018)
#define IO(a, b) (((volatile unsigned *[]){ (unsigned *)a, (unsigned *)b })[hwtype()])

#define REAL_SCREEN_BASE_ADDRESS (*(void **)0xC0000010)

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#endif /* WINSPIRE_NDLESS_STUB_LIBNDLS_H */
