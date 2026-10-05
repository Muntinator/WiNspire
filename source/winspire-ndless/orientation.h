#ifndef WINSPIRE_ORIENTATION_H
#define WINSPIRE_ORIENTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Panel orientation, shared by the calculator frontend and the host self test.
 *
 * Which of the four corrections a unit needs is a property of how its LCD is
 * mounted. It cannot be read from the Ndless API, and it has now been guessed
 * wrongly four times in released binaries - so it is measured on the unit
 * instead (the D-pad centre button cycles it, main.c) and the mapping lives
 * here where a host test can check it against the full-frame result.
 *
 * The TI-Nspire OS's bottom-left drawing origin describes how the OS issues
 * drawing commands, not the order in which lcd_blit() fills panel memory.
 * Conflating the two is what produced a left-right mirror where a vertical
 * flip was intended, so it is recorded here and must not be used as evidence.
 *
 * The flag bits compose: ORIENT_FLIP_V reverses the rows and ORIENT_FLIP_H
 * reverses the columns, so ORIENT_ROT_180 is both.
 */
#define ORIENT_IDENTITY 0x00
#define ORIENT_FLIP_V   0x01
#define ORIENT_FLIP_H   0x02
#define ORIENT_ROT_180  (ORIENT_FLIP_V | ORIENT_FLIP_H)
/* No panel direction is assumed: the calculator can select one at runtime. */
#define ORIENT_DEFAULT ORIENT_IDENTITY

/* Pixel the overlays are drawn in: white on the CX's 16bpp panel. */
#define ORIENT_OVERLAY_PIXEL 0xFFFF

/*
 * Write the guest surface to dst with the correction in op applied.
 *
 * dst must hold width * height pixels and must not overlap src. Note this is
 * its own inverse for every value of op, which is what makes cycling through
 * the settings and stopping at the one that looks right terminate on a
 * correct answer rather than on a symmetric one.
 */
static inline void orient_transform(const uint16_t *src, uint16_t *dst,
				     int width, int height, int op)
{
	int row;

	for (row = 0; row < height; row++) {
		int sy = (op & ORIENT_FLIP_V) ? height - 1 - row : row;
		const uint16_t *in = src + (size_t)sy * width;
		uint16_t *out = dst + (size_t)row * width;

		if (op & ORIENT_FLIP_H) {
			int col;

			for (col = 0; col < width; col++)
				out[col] = in[width - 1 - col];
		} else {
			memcpy(out, in, (size_t)width * sizeof(uint16_t));
		}
	}
}

/*
 * Corner brackets drawn on top of the presented surface.
 *
 * Each corner gets a different pair of leg lengths, so one photograph
 * identifies which corner is which and therefore exactly which correction the
 * unit needs. The brackets are drawn *after* the transform and are reflected
 * by op as well, so each bracket still sits in the corner of the guest screen
 * it belongs to once the whole surface has been turned.
 */
static inline void orient_draw_marker(uint16_t *surface, int width,
				      int height, int op)
{
	/*
	 * A local array rather than a file-scope const one: the corner
	 * positions depend on width and height, which are runtime arguments.
	 */
	const struct {
		int x;
		int y;
		int dx;
		int dy;
		int h;
		int v;
	} corners[4] = {
		{ 0,                0,                1,  1, 28, 20 },
		{ width - 1,        0,               -1,  1, 12, 20 },
		{ 0,                height - 1,       1, -1, 28, 10 },
		{ width - 1,        height - 1,      -1, -1, 12, 10 },
	};
	unsigned int index;

	for (index = 0; index < 4; index++) {
		int px = (op & ORIENT_FLIP_H) ?
			width - 1 - corners[index].x : corners[index].x;
		int py = (op & ORIENT_FLIP_V) ?
			height - 1 - corners[index].y : corners[index].y;
		int step;
		int dx = (op & ORIENT_FLIP_H) ? -corners[index].dx :
			corners[index].dx;
		int dy = (op & ORIENT_FLIP_V) ? -corners[index].dy :
			corners[index].dy;

		for (step = 0; step < corners[index].h; step++) {
			int x = px + dx * step;

			surface[(size_t)py * width + x] =
				ORIENT_OVERLAY_PIXEL;
			surface[(size_t)(py + dy) * width + x] =
				ORIENT_OVERLAY_PIXEL;
		}
		for (step = 0; step < corners[index].v; step++) {
			int y = py + dy * step;

			surface[(size_t)y * width + px] =
				ORIENT_OVERLAY_PIXEL;
			surface[(size_t)y * width + px + dx] =
				ORIENT_OVERLAY_PIXEL;
		}
	}
}

/*
 * A 3x5 digit at 3x scale, naming the live setting as 0-3.
 *
 * Two bars only encoded two bits ambiguously: "no bars drawn" cannot be told
 * apart from "the overlay is not running at all", which is exactly the
 * distinction needed when deciding whether a value on the calculator is being
 * honoured. A digit is unambiguous and needs no legend.
 *
 * The digit is drawn after the transform and not reflected by op, so it reads
 * the same way whatever the panel is doing - which is the point. It names the
 * setting, it does not demonstrate it.
 */
static inline void orient_draw_digit(uint16_t *surface, int width, int value)
{
	static const uint8_t glyphs[4][5] = {
		{ 0x7, 0x5, 0x5, 0x5, 0x7 },
		{ 0x2, 0x6, 0x2, 0x2, 0x7 },
		{ 0x7, 0x1, 0x7, 0x4, 0x7 },
		{ 0x7, 0x1, 0x7, 0x1, 0x7 },
	};
	const uint8_t *glyph = glyphs[value & ORIENT_ROT_180];
	int row;
	int col;

	for (row = 0; row < 5; row++) {
		for (col = 0; col < 3; col++) {
			int y;
			int x;

			if (!(glyph[row] & (1 << (2 - col))))
				continue;
			for (y = 0; y < 3; y++) {
				for (x = 0; x < 3; x++)
					surface[(size_t)(2 + row * 3 + y) *
						width + 2 + col * 3 + x] =
						ORIENT_OVERLAY_PIXEL;
			}
		}
	}
}

/*
 * Cycle to the next setting. The mask keeps this inside the four real values
 * even if a corrupted configuration handed us something else, so the sequence
 * a user sees on the keypad is always the same four states.
 */
static inline int orient_next(int op)
{
	return (op + 1) & ORIENT_ROT_180;
}

#endif /* WINSPIRE_ORIENTATION_H */
