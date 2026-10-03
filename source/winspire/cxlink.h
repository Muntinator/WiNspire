/*
 * cxlink - the TI-Nspire CX <-> ESP32 I/O bridge protocol.
 *
 * Both ends include this file: the emulator side (source/winspire/cxlink.c) and
 * the ESP32 firmware (source/esp32/main/). It is deliberately freestanding -
 * only <stdint.h>, <stdbool.h> and <stddef.h> - so it compiles for ARM926
 * under Ndless, for the host typecheck build, and for ESP-IDF.
 *
 * WHY A SERIAL LINK
 * -----------------
 * The original TI-Nspire CX exposes its external I/O on the dock connector
 * (Hackspire "Connector J01"): Tx on pin 4, Rx on pin 3, GND on pin 5, at
 * 115200 8N1 TTL. That is the only general-purpose bidirectional interface the
 * calculator has besides USB, and the CX's USB port is a device port that
 * Ndless exposes through usbdi (device-side descriptors), not as a host or a
 * bulk pipe. So the bridge is a UART link and the ESP32 is a peripheral, not a
 * co-emulator: the Nspire still runs the entire PC emulator.
 *
 * BANDWIDTH BUDGET (115200 8N1 = 11520 B/s exactly)
 * --------------------------------------------------
 * cxlink frames add 9 bytes of header plus a 2-byte CRC (see
 * CXLINK_HEADER_SIZE / CXLINK_FRAME_OVERHEAD). The default audio block is 128
 * samples, so audio at 8000 Hz mono 8-bit costs about 8.6 KiB/s of the link
 * and leaves roughly 2.9 KiB/s for Ethernet. That is enough for light TCP
 * traffic but not for bulk transfer. Consequences are load-bearing in the
 * design:
 *   - Audio is fire-and-forget. It is never acknowledged, and it is dropped
 *     (oldest first) when the link cannot keep up, because a late audio sample
 *     is worthless and must never stall the guest CPU.
 *   - Audio rate/format are negotiated at runtime (AUDIO_CONFIG) so a user can
 *     trade quality for network headroom.
 *   - Network frames are the reliable channel: they are acknowledged and are
 *     never silently dropped.
 *
 * LAYERING
 * --------
 *   guest NIC (NE2000, drivers already in Windows 95)
 *        |  send_packet / ne2000_receive        <- unchanged guest interface
 *   cxlink NET_TX / NET_RX frames
 *        |
 *   cxlink frame + CRC + sequence layer
 *        |
 *   cxlink_hal_write / cxlink_hal_read           <- transport HAL
 *        |
 *   SoC UART on the dock connector  ->  ESP32 UART1
 *        |
 *   ESP32: I2S audio out, Wi-Fi L2 bridge
 */
#ifndef CXLINK_H
#define CXLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Wire format                                                         */
/* ------------------------------------------------------------------ */
/*
 * All multi-byte fields are little-endian: both ends are LE.
 *
 *   offset  size  field
 *   ------  ----  -------------------------------------------------------
 *        0     2  sync      = CXLINK_SYNC0, CXLINK_SYNC1
 *        2     1  type      = CXLINK_MSG_*
 *        3     1  flags     = CXLINK_FLAG_*
 *        4     2  sequence  per-stream counter, wraps
 *        6     2  length    payload byte count, <= CXLINK_MAX_PAYLOAD
 *        8   ...  payload
 *   8+len      2  crc16     CRC-16/CCITT-FALSE over bytes [2, 8+len)
 *
 * The CRC deliberately starts at the type byte: the sync bytes are only a
 * resynchronisation aid, so corrupting them should not count as a good frame.
 */
#define CXLINK_SYNC0 0x5Au
#define CXLINK_SYNC1 0xA5u
#define CXLINK_HEADER_SIZE 8u
#define CXLINK_CRC_SIZE 2u
#define CXLINK_FRAME_OVERHEAD (CXLINK_HEADER_SIZE + CXLINK_CRC_SIZE)

/*
 * 1500 is the Ethernet MTU. The ESP32 side needs a contiguous RX buffer per
 * frame; keeping the maximum payload at the MTU keeps that buffer bounded and
 * avoids IP fragmentation above the link.
 */
#define CXLINK_MAX_PAYLOAD 1500u
#define CXLINK_MAX_FRAME (CXLINK_MAX_PAYLOAD + CXLINK_FRAME_OVERHEAD)

/* ------------------------------------------------------------------ */
/* Message types                                                       */
/* ------------------------------------------------------------------ */
enum {
	/* Management and keepalive. */
	CXLINK_MSG_HELLO = 0x01,
	CXLINK_MSG_VERSION = 0x02,
	CXLINK_MSG_STATUS = 0x03,
	CXLINK_MSG_RESET = 0x04,
	CXLINK_MSG_PING = 0x05,
	CXLINK_MSG_PONG = 0x06,

	/* Audio path (unreliable, drop-oldest). */
	CXLINK_MSG_AUDIO_CONFIG = 0x20,
	CXLINK_MSG_AUDIO_DATA = 0x21,
	CXLINK_MSG_AUDIO_FLUSH = 0x22,
	CXLINK_MSG_AUDIO_STATUS = 0x23,

	/* Network path (reliable, acknowledged, ordered). */
	CXLINK_MSG_NET_CONFIG = 0x40,
	CXLINK_MSG_NET_TX = 0x41,
	CXLINK_MSG_NET_RX = 0x42,
	CXLINK_MSG_NET_STATUS = 0x43,

	CXLINK_MSG_ACK = 0x60,
	CXLINK_MSG_NACK = 0x61,
	CXLINK_MSG_ERROR = 0x7f,
};

/* ------------------------------------------------------------------ */
/* Flags                                                               */
/* ------------------------------------------------------------------ */
#define CXLINK_FLAG_ACK_REQ 0x01u /* sender wants an ACK/NACK for this frame */
#define CXLINK_FLAG_RETRY 0x02u   /* this is a retransmission */
#define CXLINK_FLAG_STREAM_END 0x04u /* last frame of a logical burst */
#define CXLINK_FLAG_ERROR 0x08u   /* ERROR frames: error code is byte 0 */

/* ------------------------------------------------------------------ */
/* Protocol version                                                    */
/* ------------------------------------------------------------------ */
#define CXLINK_VERSION_MAJOR 1u
#define CXLINK_VERSION_MINOR 0u

typedef struct {
	uint8_t major;
	uint8_t minor;
	uint8_t firmware[4]; /* ESP32 side build id, ascii, from STATUS/VERSION */
	uint8_t reserved[2];
} CxlinkVersion;

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */
/*
 * Sample formats. 8-bit signed is the default because the link is the
 * bottleneck; 16-bit is available when the user lowers the sample rate.
 */
enum {
	CXLINK_AUDIO_FMT_U8_S8 = 0, /* 8-bit signed, mono or stereo */
	CXLINK_AUDIO_FMT_S16 = 1,   /* 16-bit signed little-endian */
};

typedef struct {
	uint32_t sample_rate;
	uint16_t channels; /* 1 or 2 */
	uint8_t format;    /* CXLINK_AUDIO_FMT_* */
	uint8_t flags;     /* bit 0: request mute */
	uint16_t volume;   /* 0..256, 256 = unity */
	uint16_t reserved;
} CxlinkAudioConfig;

#define CXLINK_AUDIO_FLAG_MUTE 0x01u

/*
 * Default negotiated format: 8 kHz mono 8-bit, the best quality that still
 * leaves usable headroom for the network channel on a 115200 link.
 */
#define CXLINK_AUDIO_DEFAULT_RATE 8000u

typedef struct {
	uint16_t queued_frames; /* blocks currently buffered on the ESP32 */
	uint16_t underruns;
	uint16_t overruns;
	uint16_t link_errors;
} CxlinkAudioStatus;

/* ------------------------------------------------------------------ */
/* Network                                                             */
/* ------------------------------------------------------------------ */
/*
 * The ESP32 bridges at layer 2: the guest's TCP/IP stack stays inside
 * Windows 95 and the ESP32 only moves Ethernet frames. If the ESP32 firmware
 * cannot put the radio into a raw-L2 bridge mode, it must instead act as a
 * transparent proxy and still present the same NET_TX/NET_RX interface, so the
 * guest-facing NIC never changes. See NETWORK_ARCHITECTURE.md.
 *
 * The two provisioning fields ride along in NET_CONFIG because the calculator
 * is the only console the bridge has. An access point's password is a secret,
 * and the firmware's build configuration is source controlled, so the link is
 * the right place for it; empty means "leave the uplink as it is", which is what
 * a frontend that only reports link state sends.
 */
#define CXLINK_WIFI_SSID_MAX 33u     /* 32 bytes, NUL-terminated */
#define CXLINK_WIFI_PASSWORD_MAX 65u /* 64 bytes, NUL-terminated */

/*
 * Usable lengths, and why each is one byte shorter than the field above.
 *
 * Both of these end up in ESP-IDF's wifi_sta_config_t, which keeps the SSID in
 * a 32-byte array and the passphrase in a 64-byte one, and which requires both
to be NUL-terminated. A 32-character SSID or a 64-character passphrase fills
 * that array exactly and leaves nowhere for the terminator. The worst outcomes
 * are silent - connecting to a truncated SSID, or handing the Wi-Fi stack a
 * string that runs on into the next field - so the pair is refused up front by
 * cxlink_wifi_check(), which runs on both ends of the link.
 */
#define CXLINK_WIFI_SSID_MAX_LEN 31u
#define CXLINK_WIFI_PASSWORD_MAX_LEN 63u
#define CXLINK_WIFI_PASSWORD_MIN_LEN 8u /* WPA2-PSK passphrase minimum */

typedef struct {
	uint8_t mac[6];
	uint8_t flags; /* CXLINK_NET_FLAG_*; the bridge's report */
	uint8_t state; /* CXLINK_NET_STATE_*; the bridge's report */
	uint8_t ssid[CXLINK_WIFI_SSID_MAX];
	uint8_t password[CXLINK_WIFI_PASSWORD_MAX];
} CxlinkNetConfig;

/*
 * Liveness and the uplink are two different things, and conflating them is a
 * bug in both directions: audio must keep flowing when the ESP32 has no Wi-Fi
 * association at all, and the guest's DHCP has to work before there is any
 * uplink to forward to. So:
 *
 *   - the bridge is alive when frames are arriving from it at all. cxlink.c
 *     tracks that itself, from the arrival, and cxlink_link_up() reports it.
 *   - CXLINK_NET_FLAG_UPLINK_UP is the bridge's *uplink* (its Wi-Fi station is
 *     associated and addressed). It is reported for display and for deciding
 *     whether the guest's off-subnet traffic can go anywhere; it is not what
 *     cxlink_link_up() means.
 */
#define CXLINK_NET_FLAG_UPLINK_UP 0x01u
/*
 * The bridge holds credentials. This is how provisioning is acknowledged: the
 * channel is best effort, so the calculator keeps sending until the bridge says
 * it has them, and sends again if a later report says it does not (a restarted
 * bridge reports "none", which is exactly the recovery path).
 */
#define CXLINK_NET_FLAG_CREDENTIALS 0x02u

/*
 * Uplink state as the bridge sees it. Distinguishing "rejected" from "not
 * there yet" is what tells a user with a typo in the password apart from one
 * whose access point is simply out of range.
 */
typedef enum {
	CXLINK_NET_STATE_IDLE = 0,        /* no credentials held */
	CXLINK_NET_STATE_ASSOCIATING = 1, /* credentials applied, not up yet */
	CXLINK_NET_STATE_CONNECTED = 2,   /* uplink associated and addressed */
	CXLINK_NET_STATE_REJECTED = 3,    /* the access point refused them */
} CxlinkNetState;

/*
 * Credential validation, shared by both ends so a pair that the calculator
 * accepts is never one the bridge has to refuse.
 *
 * An empty password with a non-empty SSID is legal: that is an open network,
 * which ESP-IDF is configured to accept (threshold.authmode = WIFI_AUTH_OPEN).
 */
typedef enum {
	CXLINK_WIFI_OK = 0,
	CXLINK_WIFI_NO_SSID,              /* nothing to provision */
	CXLINK_WIFI_PASSWORD_WITHOUT_SSID,
	CXLINK_WIFI_SSID_TOO_LONG,
	CXLINK_WIFI_PASSWORD_TOO_SHORT,
	CXLINK_WIFI_PASSWORD_TOO_LONG,
	CXLINK_WIFI_BAD_CHARACTER,
} CxlinkWifiResult;

/*
 * Validate a NUL-terminated SSID/password pair. Never reads more than
 * CXLINK_WIFI_SSID_MAX / CXLINK_WIFI_PASSWORD_MAX bytes, so the fixed-size wire
 * fields can be passed straight in. NULL is treated as "not supplied".
 */
CxlinkWifiResult cxlink_wifi_check(const char *ssid, const char *password);

/* Human-readable reason for a failed check. Never NULL. */
const char *cxlink_wifi_result_text(CxlinkWifiResult result);

/*
 * Give the bridge the uplink credentials. The frontend calls this once, with
 * whatever its configuration holds; the frame is then re-sent from cxlink_poll()
 * until the bridge confirms it (CXLINK_NET_FLAG_CREDENTIALS), so a lost frame or
 * a bridge that was re-plugged mid-session is recovered without the frontend
 * knowing anything about it.
 *
 * Returns the validation result. Credentials are only stored and sent on
 * CXLINK_WIFI_OK; anything else clears the pending state and sends nothing.
 * Calling this again replaces the previous pair.
 */
CxlinkWifiResult cxlink_net_provision(const char *ssid, const char *password);

/*
 * Forget the credentials: stops the retransmission and zeroes the stored
 * password. The frontend calls this on the way out so a secret that is no
 * longer needed does not sit in RAM after the emulator exits. Note that this
 * does NOT clear the bridge: the bridge keeps the credentials until it is
 * power-cycled, which is deliberate (its copy is RAM-only, and re-provisioning
 * is automatic on the next session).
 */
void cxlink_net_provision_clear(void);

typedef struct {
	bool pending;   /* stored and not yet acknowledged */
	bool confirmed; /* the bridge reported it holds them */
	uint8_t flags;  /* last CXLINK_NET_FLAG_* from the bridge */
	uint8_t state;  /* last CXLINK_NET_STATE_* from the bridge */
	uint32_t sends; /* NET_CONFIG frames transmitted, for diagnostics */
} CxlinkProvisionStatus;

void cxlink_net_provision_status(CxlinkProvisionStatus *status);

typedef struct {
	uint32_t tx_frames;
	uint32_t rx_frames;
	uint16_t tx_dropped;
	uint16_t link_errors;
} CxlinkNetStatus;

/* ------------------------------------------------------------------ */
/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection)        */
/* ------------------------------------------------------------------ */
/*
 * CRC-16/CCITT is used rather than CRC-32 because it is table-free in ROM,
 * costs one 256-entry table (or a nibble table) instead of a 1 KiB one, and is
 * sufficient for a short, framed, retransmitted link.
 */
uint16_t cxlink_crc16_update(uint16_t crc, const uint8_t *data, size_t length);
uint16_t cxlink_crc16(const uint8_t *data, size_t length);

/* ------------------------------------------------------------------ */
/* Frame codec                                                         */
/* ------------------------------------------------------------------ */
/*
 * The encoder/decoder is transport-agnostic and is shared verbatim by both
 * ends. The decoder is a byte-at-a-time state machine so it can be driven
 * straight from a UART interrupt with no buffering assumptions; a frame that
 * overruns the reassembly buffer or fails CRC is dropped and the machine
 * resynchronises on the next SYNC0/SYNC1 pair.
 */
typedef struct {
	uint8_t type;
	uint8_t flags;
	uint16_t sequence;
	uint16_t length;
	uint8_t payload[CXLINK_MAX_PAYLOAD];
} CxlinkFrame;

typedef struct {
	unsigned int state;
	unsigned int index;
	uint16_t running_crc;
	CxlinkFrame frame;
	uint32_t frames_ok;
	uint32_t frames_dropped_crc;
	uint32_t frames_dropped_format;
	uint32_t bytes_seen;
} CxlinkDecoder;

void cxlink_decoder_init(CxlinkDecoder *decoder);

/*
 * Feed one byte. Returns true when *frame was completed and validated.
 * The frame contents are valid until the next call.
 */
bool cxlink_decoder_push(CxlinkDecoder *decoder, uint8_t byte,
			 const CxlinkFrame **frame);

/* Encode one frame into out. Returns the byte count, or 0 if it does not fit. */
size_t cxlink_encode(uint8_t type, uint8_t flags, uint16_t sequence,
		     const uint8_t *payload, size_t length,
		     uint8_t *out, size_t out_size);

/* ------------------------------------------------------------------ */
/* Transport HAL                                                       */
/* ------------------------------------------------------------------ */
/*
 * Implemented by cxlink.c for the calculator (SoC UART on the dock connector)
 * and by the ESP32 firmware (UART1). Both are non-blocking: a short write must
 * never spin waiting for the far end.
 */
typedef struct {
	/* Queue up to length bytes; returns bytes accepted (may be < length). */
	size_t (*write)(void *context, const uint8_t *data, size_t length);
	/* Read available bytes; returns bytes read, 0 if none. */
	size_t (*read)(void *context, uint8_t *data, size_t capacity);
	/* Bytes currently accepted-but-unsent, to drive AUDIO_STATUS/backpressure. */
	size_t (*pending)(void *context);
} CxlinkHal;

/* ------------------------------------------------------------------ */
/* Nspire-side API (source/winspire/cxlink.c)                          */
/* ------------------------------------------------------------------ */

/* Bring up the bridge. cxlink_start_null() runs with no transport, which keeps
 * the rest of the system testable when no dock adapter is attached. */
void cxlink_init(const CxlinkHal *hal, void *hal_context);
void cxlink_start_null(void);
/*
 * Start with whichever transport this build has: the SoC UART when
 * WINSPIRE_CXLINK_UART is compiled in, otherwise nothing. Frontends call this
 * and never need to know which backend won.
 */
void cxlink_start_default(void);
#if defined(BUILD_NSPIRE) && defined(WINSPIRE_CXLINK_UART)
void cxlink_start_uart(void);
#endif

/* Periodic service. Called once per emulation batch by the frontend. */
void cxlink_poll(void);

/*
 * True while the bridge is answering: set by any frame that arrives from it and
 * cleared only by a RESET, by exhausting the reliable channel's retries, or by
 * a silence longer than CXLINK_BRIDGE_TIMEOUT_MS. This is deliberately not tied
 * to the bridge's uplink state - see CXLINK_NET_FLAG_UPLINK_UP - because audio
 * and the guest's DHCP both have to work before any Wi-Fi association exists.
 */
bool cxlink_link_up(void);
void cxlink_get_status(CxlinkNetStatus *net, CxlinkAudioStatus *audio);

/* Network path, driven by the NE2000 backend in ne2000.c. */
bool cxlink_net_send_frame(const uint8_t *frame, size_t length);
int cxlink_net_poll_frame(uint8_t *buffer, size_t capacity);

/* Audio path, fed by the shared PCM mixer. Never blocks. */
void cxlink_audio_configure(const CxlinkAudioConfig *config);
/*
 * Send sample_count mono 8-bit signed samples, already resampled by the caller
 * to cxlink_audio_rate(). Never blocks and never fails visibly: AUDIO_DATA is
 * unacknowledged and dropped oldest-first under backpressure.
 */
void cxlink_audio_write(const int8_t *pcm, size_t sample_count);
void cxlink_audio_flush(void);
/* Negotiated output rate, so the caller knows what to resample to. */
uint32_t cxlink_audio_rate(void);

/*
 * The rate the shared mixer (sb16.c / adlib.c) runs at. Not configurable in
 * this tree, and not the same as the negotiated link rate.
 */
#define CXLINK_MIXER_RATE 44100u

/*
 * How long a silent bridge is still considered present. The firmware reports
 * NET_CONFIG every 2 s and answers a keepalive PING, so four missed reports is
 * an unplugged lead or a hung bridge rather than a busy one.
 */
#define CXLINK_BRIDGE_TIMEOUT_MS 8000u

/* Samples per AUDIO_DATA frame. */
#define CXLINK_AUDIO_BLOCK 128u

/*
 * Frontend-side resampler: mixer output (s16 stereo at CXLINK_MIXER_RATE) down
 * to mono s8 at the negotiated rate, framed in CXLINK_AUDIO_BLOCK blocks.
 *
 * It is stateful on purpose, for two reasons that both cost real bandwidth if
 * ignored:
 *
 *   - The fractional sample position must carry across calls. 44100 -> 8000
 *     emits 0.1814 samples per input frame, so truncating per call would run
 *     audio slow and audibly wrong.
 *   - A partial block must be held, not framed. A frontend service tick can be
 *     as short as a millisecond (about 8 samples at 8000 Hz), and framing that
 *     on its own spends 10 header bytes on 8 bytes of audio: 18 KB/s, which
 *     does not fit the 11.5 KB/s link. Buffering to the block size holds the
 *     on-wire cost at ~1.08 bytes per sample regardless of tick rate.
 */
typedef struct {
	uint32_t accum;  /* fractional sample position, carried across calls */
	size_t pending;  /* samples buffered below, not yet framed */
	int8_t buffer[CXLINK_AUDIO_BLOCK];
} CxlinkAudioResampler;

void cxlink_audio_resampler_init(CxlinkAudioResampler *resampler);

/*
 * Feed one mixer block. Pass frames == 0 and flush == true to emit a held
 * partial block at shutdown. Returns the number of samples forwarded.
 */
size_t cxlink_audio_resampler_feed(CxlinkAudioResampler *resampler,
				   const int16_t *stereo, size_t frames, bool flush);

/*
 * Drives the codec and both channel state machines against an in-memory
 * transport. Only built when the caller asks for it, so the .tns stays lean;
 * see source/host/main.c --selftest.
 */
#ifdef CXLINK_ENABLE_SELFTEST
bool cxlink_selftest(void);
/* Line number of the check that failed, or 0. Set by cxlink_selftest(). */
extern int cxlink_selftest_failure_line;
#endif

#endif /* CXLINK_H */
