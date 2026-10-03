SHELL := /bin/bash

.PHONY: all help server native cx esp32 host bench check-cx check selftest bench-run clean

all: help

help:
	@echo "WiNspire / nspire95-cx build targets"
	@echo
	@echo "  make cx         Build nspire95-cx.tns for the original CX (needs the Ndless SDK)"
	@echo "  make cx-debug   Same, DEBUG profile (maximum tracing)"
	@echo "  make check-cx   Type-check the CX frontend + core without the SDK (host compiler)"
	@echo "  make host       Build the headless host frontend (development workstation)"
	@echo "  make bench      Assemble the x86 benchmark payload floppy image"
	@echo "  make selftest   Run the cxlink bridge self test (codec, ack, audio framing)"
	@echo "  make bench      Run the CPU benchmark on the host"
	@echo "  make esp32      Build the ESP32 bridge firmware (needs ESP-IDF; see source/esp32/README.md)"
	@echo "  make native     Build the CX II native .tns (legacy WiNspire target)"
	@echo "  make server     Build the LinuxLoader2 Server payload (Windows interop)"
	@echo "  make clean      Remove all build output"

# --- original TI-Nspire CX -------------------------------------------------
cx: bench
	@bash source/build-scripts/build_cx.sh TURBO
	@echo "CX build: build/CX/nspire95-cx.tns"

cx-debug: bench
	@bash source/build-scripts/build_cx.sh DEBUG

cx-release: bench
	@bash source/build-scripts/build_cx.sh RELEASE

check-cx:
	@bash source/build-scripts/check_cx_frontend.sh . DEBUG
	@bash source/build-scripts/check_cx_frontend.sh . RELEASE
	@bash source/build-scripts/check_cx_frontend.sh . TURBO

# --- development host ------------------------------------------------------
host:
	@bash source/build-scripts/build_host.sh
	@echo "Host build: build/Host/winspire-host"

bench:
	@bash source/build-scripts/build_bench.sh

bench-run: host bench
	@build/Host/winspire-host --boot build/bench/bench386.img --seconds 60

# --- verification ----------------------------------------------------------
# The self test is the only executable check of the ESP32 bridge protocol: the
# calculator frontend can only be type-checked here (see check-cx).
selftest: host
	@build/Host/winspire-host --selftest

check: check-cx selftest

# --- ESP32 bridge firmware ------------------------------------------------
# Lives in source/esp32/ rather than build-scripts/ because ESP-IDF must run
# from its project directory. Set IDF_PATH if your ESP-IDF is not at /tmp/esp-idf.
esp32:
	@bash source/esp32/build.sh
	@echo "ESP32 firmware: build/esp32/cxlink-bridge.bin"

# --- legacy targets --------------------------------------------------------
server:
	@command -v arm-linux-gnueabi-gcc >/dev/null || { echo "Missing arm-linux-gnueabi-gcc" >&2; exit 1; }
	@command -v powershell.exe >/dev/null || { echo "Missing Windows PowerShell interop" >&2; exit 1; }
	@bash source/build-scripts/build_server.sh >/dev/null
	@script="$$(wslpath -w "$(CURDIR)/source/server/build_netblock_server.ps1")"; powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "$$script" >/dev/null
	@echo "Server build: build/Server"

native:
	@bash source/build-scripts/build_native.sh >/dev/null
	@echo "Native build: build/Native"

clean:
	@rm -rf build
	@echo "Removed WiNspire build output"
