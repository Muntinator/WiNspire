#ifndef INPUT_SELFTEST_H
#define INPUT_SELFTEST_H

#include <stdbool.h>

/*
 * Checks the calculator frontend's key handling end to end: the Fn chord that
 * supplies F1-F15 on a keypad that has none, Ctrl and Alt, and that nothing
 * sticks or leaks. It links the real PS/2 code, so the scancodes it asserts
 * are the ones a guest would actually read from port 0x60.
 *
 * Host only (it stands in for the calculator's keypad, which does not exist
 * here); run through `build/Host/winspire-host --selftest`.
 */
bool input_selftest(void);

/* Set by input_selftest() when a check fails, so the caller can name it. */
extern int input_selftest_failure_line;

#endif /* INPUT_SELFTEST_H */
