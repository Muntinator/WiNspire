/*
 * Minimal host stand-in for the Ndless SDK header.
 *
 * The shared core is compiled with BUILD_NSPIRE on the calculator so that the
 * Ndless-specific paths (ARM fast paths, bulk REP, Nspire timer/IDE behaviour)
 * are the ones being built and measured. The host frontend wants that same
 * configuration, but the Ndless SDK is not present outside a calculator build.
 *
 * TINY386_NO_LOG (set by release_config.h for the native target) turns every
 * nspire_log() call site into a no-op, so the only thing the core actually
 * needs from libndls.h to compile is the header itself.
 *
 * This file is only ever on the include path of build_host.sh. Calculator
 * builds continue to resolve libndls.h from the real Ndless SDK.
 */
#ifndef WINSPIRE_HOST_LIBNDLS_SHIM_H
#define WINSPIRE_HOST_LIBNDLS_SHIM_H

#include <stdbool.h>
#include <stdint.h>

#endif /* WINSPIRE_HOST_LIBNDLS_SHIM_H */
