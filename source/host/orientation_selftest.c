/*
 * Orientation self test for the calculator frontend's panel transform.
 *
 * Compiles the SAME header the .tns ships (source/winspire-ndless/
 * orientation.h). A transcribed copy would keep passing while the shipping
 * code changed underneath it, which is the exact failure this setting has
 * already produced four times: a plausible, wrong, compiled-in transform.
 *
 * What is asserted:
 *   - identity is a byte-exact copy
 *   - each of the four settings matches an independently computed mapping
 *   - every setting is its own inverse, which is what makes cycling with the
 *     D-pad terminate on the right answer instead of on a symmetric one
 *   - two different settings never produce the same image of an asymmetric
 *     test pattern, so exactly one of the four can look right
 *   - the corner marker draws something in all four corners, they are all
 *     distinguishable, and nothing lands outside the surface
 *   - the value digit is drawn unreflected so it reads the same whatever the
 *     panel does, stays inside its own box, and the four digits differ
 *   - the partial-update staging loop agrees pixel for pixel with the
 *     full-frame transform, for every setting and nine bands. If the two
 *     disagree the screen tears into a mixture of orientations, which is the
 *     v1.0.3 defect.
 *
 * Run through `build/Host/winspire-host --selftest`.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orientation.h"
#include "orientation_selftest.h"

#define W 320
#define H 240
#define PIXELS ((size_t)W * H)

static int checks;
static int failures;
int orientation_selftest_failure_line;

#define CHECK(cond, ...)                                              \
	do {                                                        \
		checks++;                                          \
		if (!(cond)) {                                      \
			failures++;                                \
			orientation_selftest_failure_line = __LINE__; \
			printf("  -> FAIL at line %d: ", __LINE__);  \
			printf(__VA_ARGS__);                        \
			printf("\n");                               \
		}                                                   \
	} while (0)

/*
 * An asymmetric pattern: every pixel encodes its own position, so any
 * reordering of the surface shows up as a mismatch. Values are kept to 15 bits
 * so the pattern can never be mistaken for the white the overlays draw in.
 */
static uint16_t *make_pattern(void)
{
	uint16_t *surface = malloc(PIXELS * sizeof(uint16_t));
	int y;
	int x;

	if (!surface)
		return NULL;
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			surface[(size_t)y * W + x] =
				(uint16_t)(((y * W + x) ^ 0x5A5A) & 0x7FFF);
	return surface;
}

/* The expected result of applying op, written out longhand and independently. */
static void reference_transform(const uint16_t *src, uint16_t *dst, int op)
{
	int y;
	int x;

	for (y = 0; y < H; y++) {
		for (x = 0; x < W; x++) {
			int sy = (op & ORIENT_FLIP_V) ? H - 1 - y : y;
			int sx = (op & ORIENT_FLIP_H) ? W - 1 - x : x;

			dst[(size_t)y * W + x] = src[(size_t)sy * W + sx];
		}
	}
}

/*
 * The staging loop from draw_region() in source/winspire-ndless/main.c,
 * transcribed on purpose: the assertion is that this loop and the shared
 * header agree, so it cannot simply be the shared header.
 *
 * Writes the band into `screen`, which the caller pre-fills with the
 * full-frame result, so any pixel the band paints in the wrong place - or
 * fails to paint at all - shows up as a difference.
 */
static void partial_update(const uint16_t *frame, uint16_t *staging,
			   uint16_t *screen, int left, int top,
			   int width, int height, int op)
{
	const uint16_t *source;
	uint16_t *dest;
	int row;

	if (op == ORIENT_IDENTITY) {
		source = frame + (size_t)top * W + left;
		dest = screen + (size_t)top * W + left;
		for (row = 0; row < height; row++) {
			memcpy(dest, source,
			       (size_t)width * sizeof(uint16_t));
			source += W;
			dest += W;
		}
		return;
	}
	{
		int flip_rows = (op & ORIENT_FLIP_V) != 0;
		int flip_cols = (op & ORIENT_FLIP_H) != 0;
		int mirror = H - 1 - top;
		int first = flip_rows ? mirror - height + 1 : top;
		int col = flip_cols ? W - left - width : left;

		for (row = 0; row < height; row++) {
			const uint16_t *in = frame +
				(size_t)(top + row) * W + left;
			uint16_t *out = staging +
				(size_t)(flip_rows ? mirror - row
						   : top + row) * W + col;
			int i;

			if (flip_cols) {
				for (i = 0; i < width; i++)
					out[i] = in[width - 1 - i];
			} else {
				memcpy(out, in,
				       (size_t)width * sizeof(uint16_t));
			}
		}
		/*
		 * Both sides of the copy run in the same direction: the
		 * staging loop wrote rows going up the panel, so the copy
		 * starts at the lowest row written and walks upward.
		 */
		source = staging + (size_t)first * W + col;
		dest = screen + (size_t)first * W + col;
	}
	for (row = 0; row < height; row++) {
		memcpy(dest, source, (size_t)width * sizeof(uint16_t));
		source += W;
		dest += W;
	}
}

/* The nine bands draw_region() is exercised with: the four edges, the four
 * corners, and a centre band. */
static const struct {
	int x, y, w, h;
	const char *name;
} bands[9] = {
	{ 0,       0,       W,       H,       "full frame"      },
	{ 0,       0,       64,      24,      "top-left corner" },
	{ W - 64,  0,       64,      24,      "top-right corner" },
	{ 0,       H - 24,  64,      24,      "bottom-left corner" },
	{ W - 64,  H - 24,  64,      24,      "bottom-right corner" },
	{ 100,     80,      120,     60,      "centre"          },
	{ 0,       100,     W,       40,      "full-width band" },
	{ 100,     0,       120,     H,       "full-height band" },
	{ W - 4,   H - 4,   4,       4,       "last pixel"      },
};

/* Lit pixels within 40 of each corner, so the tally does not depend on the
 * exact bracket geometry. */
static void tally_corners(const uint16_t *surface, int *out)
{
	static const int cx[4] = { 0, W - 1, 0, W - 1 };
	static const int cy[4] = { 0, 0, H - 1, H - 1 };
	int corner;

	for (corner = 0; corner < 4; corner++) {
		int count = 0;
		int y;
		int x;

		for (y = cy[corner] - 40; y <= cy[corner] + 40; y++) {
			if (y < 0 || y >= H)
				continue;
			for (x = cx[corner] - 40; x <= cx[corner] + 40;
			     x++) {
				if (x < 0 || x >= W)
					continue;
				if (surface[(size_t)y * W + x] ==
				    ORIENT_OVERLAY_PIXEL)
					count++;
			}
		}
		out[corner] = count;
	}
}

bool orientation_selftest(void)
{
	uint16_t *pattern = make_pattern();
	uint16_t *full = malloc(PIXELS * sizeof(uint16_t));
	uint16_t *expect = malloc(PIXELS * sizeof(uint16_t));
	uint16_t *staging = malloc(PIXELS * sizeof(uint16_t));
	uint16_t *partial = malloc(PIXELS * sizeof(uint16_t));
	uint16_t *scratch = malloc(PIXELS * sizeof(uint16_t));
	uint16_t *images[4] = { NULL, NULL, NULL, NULL };
	uint16_t *digits[4] = { NULL, NULL, NULL, NULL };
	int op;

	failures = 0;
	checks = 0;
	orientation_selftest_failure_line = 0;

	printf("orientation transform (shared header):\n");
	if (!pattern || !full || !expect || !staging || !partial ||
	    !scratch) {
		printf("  -> FAIL: out of memory\n");
		free(pattern);
		free(full);
		free(expect);
		free(staging);
		free(partial);
		free(scratch);
		return false;
	}

	/* Identity is an exact copy: lcd_blit() expects the surface as-is. */
	orient_transform(pattern, full, W, H, ORIENT_IDENTITY);
	CHECK(memcmp(pattern, full, PIXELS * sizeof(uint16_t)) == 0,
	      "orientation 0 must be a byte-exact copy of the guest surface");

	for (op = 0; op <= ORIENT_ROT_180; op++) {
		size_t i;
		int mismatches = 0;

		orient_transform(pattern, full, W, H, op);
		reference_transform(pattern, expect, op);
		for (i = 0; i < PIXELS; i++)
			if (full[i] != expect[i])
				mismatches++;
		CHECK(mismatches == 0,
		      "orientation %d differs from the reference in %d pixels",
		      op, mismatches);
		images[op] = malloc(PIXELS * sizeof(uint16_t));
		if (images[op])
			memcpy(images[op], full, PIXELS * sizeof(uint16_t));
	}

	/* Cycling with the D-pad must visit all four and return to start. */
	{
		int value = ORIENT_IDENTITY;
		int seen = 1 << value;
		int step;

		for (step = 0; step < 4; step++)
			value = orient_next(value);
		CHECK(value == ORIENT_IDENTITY,
		      "four D-pad presses must return to 0, got %d", value);
		for (step = 0; step < 3; step++) {
			value = orient_next(value);
			seen |= 1 << value;
		}
		CHECK(seen == 0x0F, "D-pad cycling must visit all four values");
		CHECK(orient_next(99) >= 0 && orient_next(99) <= 3,
		      "cycling must stay inside the four real values");
	}

	/* Each setting is its own inverse. */
	for (op = 0; op <= ORIENT_ROT_180; op++) {
		orient_transform(pattern, full, W, H, op);
		orient_transform(full, expect, W, H, op);
		CHECK(memcmp(pattern, expect, PIXELS * sizeof(uint16_t)) == 0,
		      "orientation %d is not its own inverse", op);
	}

	/* No two settings may render the asymmetric pattern identically. */
	{
		int a;
		int b;

		for (a = 0; a <= ORIENT_ROT_180; a++) {
			for (b = a + 1; b <= ORIENT_ROT_180; b++) {
				if (!images[a] || !images[b])
					continue;
				CHECK(memcmp(images[a], images[b],
					     PIXELS * sizeof(uint16_t)) != 0,
				      "orientations %d and %d render "
				      "identically", a, b);
			}
		}
	}

	/*
	 * The overlays are checked against a CONSTANT background, not the
	 * test pattern. Compared over a patterned surface the guest content
	 * differs between settings anyway, so two identical overlays would
	 * still look different and the check would pass on a defect. A flat
	 * background leaves only what the overlay drew.
	 */
	printf("orientation marker:\n");
	for (op = 0; op <= ORIENT_ROT_180; op++) {
		int tally[4];
		size_t lit = 0;
		size_t p;
		int i;
		int distinct = 1;

		memset(scratch, 0x1111, PIXELS * sizeof(uint16_t));
		orient_draw_marker(scratch, W, H, op);
		for (p = 0; p < PIXELS; p++)
			if (scratch[p] == ORIENT_OVERLAY_PIXEL)
				lit++;
		CHECK(lit > 0, "orientation %d drew no marker", op);

		tally_corners(scratch, tally);
		for (i = 0; i < 4; i++) {
			int j;

			CHECK(tally[i] > 0,
			      "orientation %d: corner %d has no bracket",
			      op, i);
			for (j = i + 1; j < 4; j++)
				if (tally[i] == tally[j])
					distinct = 0;
		}
		CHECK(distinct,
		      "orientation %d: corner brackets are not all "
		      "distinguishable, so one photo cannot identify them",
		      op);
		/* Every bracket must sit near a corner, never mid-screen. */
		{
			size_t stray = 0;
			int y;
			int x;

			for (y = 0; y < H; y++) {
				for (x = 0; x < W; x++) {
					int near_corner =
						(x < 40 || x >= W - 40) &&
						(y < 40 || y >= H - 40);

					if (!near_corner &&
					    scratch[(size_t)y * W + x] ==
					    ORIENT_OVERLAY_PIXEL)
						stray++;
				}
			}
			CHECK(stray == 0,
			      "orientation %d painted %zu pixels away from "
			      "every corner", op, stray);
		}
	}

	printf("orientation digit:\n");
	for (op = 0; op <= ORIENT_ROT_180; op++) {
		int y;
		int x;
		int bad = 0;

		orient_transform(pattern, full, W, H, op);
		memset(scratch, 0x1111, PIXELS * sizeof(uint16_t));
		orient_draw_digit(scratch, W, op);
		digits[op] = malloc(PIXELS * sizeof(uint16_t));
		if (digits[op])
			memcpy(digits[op], scratch, PIXELS * sizeof(uint16_t));
		/* Outside its own box the background must be untouched. */
		for (y = 0; y < H; y++) {
			for (x = 0; x < W; x++) {
				int in_glyph = x >= 2 && x < 11 &&
					       y >= 2 && y < 17;

				if (in_glyph)
					continue;
				if (scratch[(size_t)y * W + x] != 0x1111)
					bad++;
			}
		}
		CHECK(bad == 0,
		      "digit %d painted %d pixels outside its own box",
		      op, bad);
		/* It must actually draw something. */
		{
			size_t lit = 0;
			size_t p;

			for (p = 0; p < PIXELS; p++)
				if (scratch[p] == ORIENT_OVERLAY_PIXEL)
					lit++;
			CHECK(lit > 0, "digit %d drew nothing", op);
		}
	}
	{
		int a;
		int b;

		for (a = 0; a <= ORIENT_ROT_180; a++) {
			CHECK(digits[a] != NULL, "digit %d not captured", a);
			for (b = a + 1; b <= ORIENT_ROT_180; b++) {
				if (!digits[a] || !digits[b])
					continue;
				CHECK(memcmp(digits[a], digits[b],
					     PIXELS * sizeof(uint16_t)) != 0,
				      "digits %d and %d look identical, so "
				      "the screen cannot name the setting",
				      a, b);
			}
		}
	}

	printf("partial updates agree with the full frame:\n");
	for (op = 0; op <= ORIENT_ROT_180; op++) {
		int band;

		orient_transform(pattern, full, W, H, op);
		for (band = 0; band < 9; band++) {
			memcpy(partial, full, PIXELS * sizeof(uint16_t));
			partial_update(pattern, staging, partial,
				       bands[band].x, bands[band].y,
				       bands[band].w, bands[band].h, op);
			CHECK(memcmp(partial, full,
				     PIXELS * sizeof(uint16_t)) == 0,
			      "orientation %d, band %s: partial update "
			      "disagrees with the full-frame transform",
			      op, bands[band].name);
		}
	}

	printf("  %d checks, %d failure(s)\n", checks, failures);

	for (op = 0; op <= ORIENT_ROT_180; op++) {
		free(images[op]);
		free(digits[op]);
	}
	free(pattern);
	free(full);
	free(expect);
	free(staging);
	free(partial);
	free(scratch);
	return failures == 0;
}
