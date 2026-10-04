#ifndef ORIENTATION_SELFTEST_H
#define ORIENTATION_SELFTEST_H

#include <stdbool.h>

/*
 * Checks the calculator frontend's panel orientation transform, corner marker
 * and value digit. It compiles the same header the .tns ships
 * (source/winspire-ndless/orientation.h), so the shipping code cannot change
 * underneath a passing test - the failure mode this setting has produced four
 * times already.
 *
 * Host only (there is no equivalent of a 320x240 panel to look at here); run
 * through `build/Host/winspire-host --selftest`.
 */
bool orientation_selftest(void);

/* Set by orientation_selftest() when a check fails, so the caller can name it. */
extern int orientation_selftest_failure_line;

#endif /* ORIENTATION_SELFTEST_H */
