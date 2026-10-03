/*
 * cxlink - Nspire-side implementation.
 *
 * Layers, bottom to top:
 *
 *   cxlink_hal (SoC UART on the dock connector)   <- cxlink_hal_nspire
 *   byte rings + frame decoder/encoder
 *   reliable channel (NET_*) and unreliable channel (AUDIO_*)
 *   cxlink_net_*  /  cxlink_audio_*  /  cxlink_net_provision() public API
 *
 * The ESP32 has no console of its own, so this file is also where its Wi-Fi
 * uplink credentials come from: see the provisioning section below, and
 * NETWORK_ARCHITECTURE.md 6.3 for the whole path.
 *
 * The NE2000 guest NIC plugs into cxlink_net_* from ne2000.c (see the
 * USE_CXLINK backend there), so nothing about the guest-facing hardware
 * changes: Windows 95 still drives the same NE2000 and the ESP32 is invisible
 * to it.
 *
 * NOTHING HERE BLOCKS. The x86 interpreter must never wait on the ESP32. When
 * the link is slow the audio path drops oldest-first and the network path
 * applies backpressure to the NIC (which Windows 95 already handles by
 * dropping frames at the driver level).
 */
#include "cxlink.h"

#include <string.h>

#ifdef BUILD_NSPIRE
#include <libndls.h>
#endif

/* ------------------------------------------------------------------ */
/* CRC-16/CCITT-FALSE                                                  */
/* ------------------------------------------------------------------ */

uint16_t cxlink_crc16_update(uint16_t crc, const uint8_t *data, size_t length)
{
	size_t i;
	int bit;

	for (i = 0; i < length; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (bit = 0; bit < 8; bit++)
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
					      : (uint16_t)(crc << 1);
	}
	return crc;
}

uint16_t cxlink_crc16(const uint8_t *data, size_t length)
{
	return cxlink_crc16_update(0xFFFFu, data, length);
}

/* ------------------------------------------------------------------ */
/* Frame decoder                                                       */
/* ------------------------------------------------------------------ */

enum {
	DEC_SYNC0 = 0,
	DEC_SYNC1,
	DEC_TYPE,
	DEC_FLAGS,
	DEC_SEQ_LO,
	DEC_SEQ_HI,
	DEC_LEN_LO,
	DEC_LEN_HI,
	DEC_PAYLOAD,
	DEC_CRC_LO,
	DEC_CRC_HI,
};

void cxlink_decoder_init(CxlinkDecoder *decoder)
{
	memset(decoder, 0, sizeof(*decoder));
	decoder->state = DEC_SYNC0;
}

bool cxlink_decoder_push(CxlinkDecoder *decoder, uint8_t byte,
			 const CxlinkFrame **frame)
{
	decoder->bytes_seen++;

	switch (decoder->state) {
	case DEC_SYNC0:
		if (byte == CXLINK_SYNC0)
			decoder->state = DEC_SYNC1;
		return false;

	case DEC_SYNC1:
		if (byte == CXLINK_SYNC1) {
			decoder->state = DEC_TYPE;
			decoder->running_crc = 0xFFFFu;
		} else if (byte != CXLINK_SYNC0) {
			/* Not a sync pair; stay armed for the next SYNC0. */
			decoder->state = DEC_SYNC0;
		}
		return false;

	case DEC_TYPE:
		decoder->frame.type = byte;
		decoder->running_crc = cxlink_crc16_update(decoder->running_crc,
							   &byte, 1);
		decoder->state = DEC_FLAGS;
		return false;

	case DEC_FLAGS:
		decoder->frame.flags = byte;
		decoder->running_crc = cxlink_crc16_update(decoder->running_crc,
							   &byte, 1);
		decoder->state = DEC_SEQ_LO;
		return false;

	case DEC_SEQ_LO:
		decoder->frame.sequence = byte;
		decoder->running_crc = cxlink_crc16_update(decoder->running_crc,
							   &byte, 1);
		decoder->state = DEC_SEQ_HI;
		return false;

	case DEC_SEQ_HI:
		decoder->frame.sequence |= (uint16_t)byte << 8;
		decoder->running_crc = cxlink_crc16_update(decoder->running_crc,
							   &byte, 1);
		decoder->state = DEC_LEN_LO;
		return false;

	case DEC_LEN_LO:
		decoder->frame.length = byte;
		decoder->running_crc = cxlink_crc16_update(decoder->running_crc,
							   &byte, 1);
		decoder->state = DEC_LEN_HI;
		return false;

	case DEC_LEN_HI:
		decoder->frame.length |= (uint16_t)byte << 8;
		decoder->running_crc = cxlink_crc16_update(decoder->running_crc,
							   &byte, 1);
		if (decoder->frame.length > CXLINK_MAX_PAYLOAD) {
			/*
			 * Impossible length: this is a desynchronised stream, so
			 * resynchronise rather than trusting the length field.
			 */
			decoder->frames_dropped_format++;
			decoder->state = DEC_SYNC0;
			return false;
		}
		decoder->index = 0;
		decoder->state = decoder->frame.length ? DEC_PAYLOAD : DEC_CRC_LO;
		return false;

	case DEC_PAYLOAD:
		decoder->frame.payload[decoder->index++] = byte;
		decoder->running_crc = cxlink_crc16_update(decoder->running_crc,
							   &byte, 1);
		if (decoder->index >= decoder->frame.length)
			decoder->state = DEC_CRC_LO;
		return false;

	case DEC_CRC_LO:
		decoder->frame.length = decoder->frame.length; /* keep */
		decoder->index = byte; /* borrow index as crc low byte */
		decoder->state = DEC_CRC_HI;
		return false;

	case DEC_CRC_HI:
		{
			uint16_t received = (uint16_t)((byte << 8) |
						       (decoder->index & 0xFF));

			decoder->state = DEC_SYNC0;
			if (received != decoder->running_crc) {
				decoder->frames_dropped_crc++;
				return false;
			}
			decoder->frames_ok++;
			if (frame)
				*frame = &decoder->frame;
			return true;
		}
	}

	decoder->state = DEC_SYNC0;
	return false;
}

/* ------------------------------------------------------------------ */
/* Frame encoder                                                       */
/* ------------------------------------------------------------------ */

size_t cxlink_encode(uint8_t type, uint8_t flags, uint16_t sequence,
		     const uint8_t *payload, size_t length,
		     uint8_t *out, size_t out_size)
{
	uint16_t crc;
	size_t total;

	if (length > CXLINK_MAX_PAYLOAD)
		return 0;
	total = length + CXLINK_FRAME_OVERHEAD;
	if (out_size < total)
		return 0;

	out[0] = CXLINK_SYNC0;
	out[1] = CXLINK_SYNC1;
	out[2] = type;
	out[3] = flags;
	out[4] = (uint8_t)(sequence & 0xFF);
	out[5] = (uint8_t)(sequence >> 8);
	out[6] = (uint8_t)(length & 0xFF);
	out[7] = (uint8_t)(length >> 8);
	if (length && payload)
		memcpy(out + CXLINK_HEADER_SIZE, payload, length);
	/* CRC covers type..payload, i.e. everything after the sync bytes. */
	crc = cxlink_crc16(out + 2, CXLINK_HEADER_SIZE - 2 + length);
	out[CXLINK_HEADER_SIZE + length] = (uint8_t)(crc & 0xFF);
	out[CXLINK_HEADER_SIZE + length + 1] = (uint8_t)(crc >> 8);
	return total;
}

/* ------------------------------------------------------------------ */
/* byte rings                                                          */
/* ------------------------------------------------------------------ */

/*
 * Power-of-two sizes so indexing is an AND: the ARM926 has no integer divide
 * instruction and a runtime modulo in the UART path would be a library call.
 */
#define CXLINK_TX_RING 4096u
#define CXLINK_RX_RING 2048u

typedef struct {
	uint8_t data[CXLINK_TX_RING];
	uint16_t head;
	uint16_t tail;
} CxlinkTxRing;

typedef struct {
	uint8_t data[CXLINK_RX_RING];
	uint16_t head;
	uint16_t tail;
} CxlinkRxRing;

typedef struct {
	uint16_t head;
	uint16_t tail;
} CxlinkFrameQueue;

/* ------------------------------------------------------------------ */
/* Reliable (network) channel                                          */
/* ------------------------------------------------------------------ */

/*
 * One outstanding reliable frame at a time. The link is a single serial line
 * with a bounded round trip, and Ethernet frames are already retried by the
 * guest's TCP/IP stack, so a window of one keeps the state machine small and
 * the worst-case latency predictable.
 */
#define CXLINK_NET_QUEUE 8u
#define CXLINK_RETRY_MS 40u
#define CXLINK_RETRY_MAX 6u

/*
 * How often an unacknowledged credential frame is re-sent. NET_CONFIG is a
 * best-effort message and the bridge reports its state every 2 s, so this is
 * the same cadence: a lost frame costs one retry, and a bridge that was
 * re-plugged is re-provisioned within a couple of seconds without the frontend
 * getting involved.
 */
#define CXLINK_PROVISION_RETRY_MS 2000u

typedef struct {
	uint8_t data[CXLINK_MAX_PAYLOAD];
	uint16_t length;
	uint16_t sequence;
	uint8_t retries;
	bool in_use;
} CxlinkPendingNet;

typedef struct {
	CxlinkHal hal;
	void *hal_context;

	CxlinkDecoder decoder;
	CxlinkTxRing tx_ring;
	CxlinkRxRing rx_ring;

	/* sequence counters, one per stream, as the protocol requires */
	uint16_t seq_net_tx;
	uint16_t seq_net_rx;
	uint16_t seq_audio;
	uint16_t seq_mgmt;
	uint16_t seq_net_config;

	/*
	 * Uplink provisioning. The credentials are held here rather than in the
	 * frontend because the retransmission is this layer's job; the password
	 * is zeroed by cxlink_net_provision_clear() when the frontend is done.
	 */
	uint8_t provision_ssid[CXLINK_WIFI_SSID_MAX];
	uint8_t provision_password[CXLINK_WIFI_PASSWORD_MAX];
	bool provision_pending;
	bool provision_confirmed;
	uint32_t provision_next_ms;
	uint32_t provision_sends;

	/* Last CXLINK_NET_FLAG_* / CXLINK_NET_STATE_* the bridge reported. */
	uint8_t net_flags;
	uint8_t net_state;

	/* inbound network frames awaiting the NIC */
	CxlinkFrameQueue net_rx_queue;
	uint8_t net_rx_data[CXLINK_NET_QUEUE][CXLINK_MAX_PAYLOAD];
	uint16_t net_rx_length[CXLINK_NET_QUEUE];

	/* outbound reliable frame awaiting ACK */
	CxlinkPendingNet pending;
	uint32_t pending_deadline_ms;
	uint32_t pending_not_before_ms;

	/* audio */
	CxlinkAudioConfig audio;
	bool audio_configured;
	uint32_t audio_frames_sent;
	uint16_t audio_underruns;
	uint16_t audio_dropped;

	/* link health */
	bool link_up;
	uint32_t link_errors;
	uint32_t crc_errors_last;
	/* Time of the last frame received from the bridge (liveness). */
	uint32_t mgmt_last_rx_ms;
	/* Time we last probed it, which is not the same as hearing from it. */
	uint32_t probe_last_ms;
	uint32_t net_tx_frames;
	uint32_t net_rx_frames;
	uint32_t net_tx_dropped;
} CxlinkState;

static CxlinkState cxlink;

/* Provided by the frontend, mirroring get_uticks() in pc.h. */
uint32_t get_uticks(void);

static uint32_t cxlink_now_ms(void)
{
	return get_uticks() / 1000u;
}

/* ------------------------------------------------------------------ */
/* transmit path                                                       */
/* ------------------------------------------------------------------ */

static size_t cxlink_ring_write(CxlinkTxRing *ring, const uint8_t *data,
				size_t length)
{
	size_t written = 0;

	while (written < length) {
		uint16_t next = (uint16_t)((ring->head + 1u) & (CXLINK_TX_RING - 1u));

		if (next == ring->tail)
			break; /* full */
		ring->data[ring->head] = data[written++];
		ring->head = next;
	}
	return written;
}

static size_t cxlink_ring_read(CxlinkTxRing *ring, uint8_t *data,
			       size_t capacity)
{
	size_t read = 0;

	while (read < capacity && ring->tail != ring->head) {
		data[read++] = ring->data[ring->tail];
		ring->tail = (uint16_t)((ring->tail + 1u) &
					(CXLINK_TX_RING - 1u));
	}
	return read;
}

/*
 * Flush as much of the TX ring as the HAL will take. Called once per emulation
 * batch; it never spins.
 */
static void cxlink_flush(void)
{
	uint8_t scratch[64];

	if (!cxlink.hal.write)
		return;
	for (;;) {
		size_t available = cxlink_ring_read(&cxlink.tx_ring, scratch,
						    sizeof(scratch));
		size_t accepted;

		if (!available)
			return;
		accepted = cxlink.hal.write(cxlink.hal_context, scratch,
					    available);
		if (accepted < available) {
			/*
			 * The UART FIFO is full. Push the unsent tail back so the
			 * bytes are not lost; the next flush retries them.
			 */
			size_t unsent = available - accepted;
			uint16_t rewind = (uint16_t)((cxlink.tx_ring.tail -
						      unsent) &
						     (CXLINK_TX_RING - 1u));

			cxlink.tx_ring.tail = rewind;
			return;
		}
	}
}

static void cxlink_send(uint8_t type, uint8_t flags, uint16_t sequence,
			const uint8_t *payload, size_t length)
{
	uint8_t frame[CXLINK_MAX_FRAME];
	size_t frame_length = cxlink_encode(type, flags, sequence, payload,
					    length, frame, sizeof(frame));
	size_t written;

	if (!frame_length)
		return;
	written = cxlink_ring_write(&cxlink.tx_ring, frame, frame_length);
	if (written < frame_length) {
		/*
		 * Out of TX room. This is the backpressure point: audio is
		 * dropped (the newest block is discarded, which is the cheapest
		 * correct response on a serial link), while network frames are
		 * left queued by the caller for retry.
		 */
		cxlink.tx_ring.head = cxlink.tx_ring.tail; /* drop the partial write */
		if (type == CXLINK_MSG_AUDIO_DATA)
			cxlink.audio_dropped++;
		return;
	}
	cxlink_flush();
}

static void cxlink_send_ack(uint8_t acked_type, uint8_t acked_flags,
			    uint16_t acked_sequence, uint8_t status)
{
	uint8_t payload[4];

	payload[0] = acked_type;
	payload[1] = acked_flags;
	payload[2] = (uint8_t)(acked_sequence & 0xFF);
	payload[3] = (uint8_t)(acked_sequence >> 8);
	(void)status;
	cxlink_send(status ? CXLINK_MSG_NACK : CXLINK_MSG_ACK, 0,
		    cxlink.seq_mgmt++, payload, sizeof(payload));
}

/* ------------------------------------------------------------------ */
/* network plumbing                                                    */
/* ------------------------------------------------------------------ */

static void cxlink_net_queue_push(const CxlinkFrame *frame)
{
	uint16_t next = (uint16_t)((cxlink.net_rx_queue.head + 1u) %
				   CXLINK_NET_QUEUE);

	if (next == cxlink.net_rx_queue.tail) {
		/* Queue full: drop the oldest so the newest frames survive. */
		cxlink.net_rx_queue.tail =
			(uint16_t)((cxlink.net_rx_queue.tail + 1u) %
				   CXLINK_NET_QUEUE);
		cxlink.net_tx_dropped++;
	}
	memcpy(cxlink.net_rx_data[cxlink.net_rx_queue.head], frame->payload,
	       frame->length);
	cxlink.net_rx_length[cxlink.net_rx_queue.head] = frame->length;
	cxlink.net_rx_queue.head = next;
	cxlink.net_rx_frames++;
}

/*
 * Called by the NE2000 backend when the guest transmits a frame.
 * Returns false when the frame could not be queued; the NIC treats that as a
 * lost transmit, which is what a real card does under congestion.
 */
bool cxlink_net_send_frame(const uint8_t *frame, size_t length)
{
	uint8_t out[CXLINK_MAX_FRAME];
	size_t total;

	if (!cxlink.link_up || length == 0 || length > CXLINK_MAX_PAYLOAD)
		return false;
	/*
	 * The reliable channel holds one outstanding frame, so a new frame can
	 * only start once the previous one is acknowledged.
	 */
	if (cxlink.pending.in_use)
		return false;
	total = cxlink_encode(CXLINK_MSG_NET_TX, CXLINK_FLAG_ACK_REQ,
			      cxlink.seq_net_tx, frame, length, out, sizeof(out));
	if (!total)
		return false;
	if (cxlink_ring_write(&cxlink.tx_ring, out, total) < total) {
		cxlink.tx_ring.head = cxlink.tx_ring.tail;
		cxlink.net_tx_dropped++;
		return false;
	}
	memcpy(cxlink.pending.data, frame, length);
	cxlink.pending.length = (uint16_t)length;
	cxlink.pending.sequence = cxlink.seq_net_tx;
	cxlink.pending.retries = 0;
	cxlink.pending.in_use = true;
	cxlink.pending_not_before_ms = cxlink_now_ms() + CXLINK_RETRY_MS;
	cxlink.pending_deadline_ms = cxlink_now_ms() + CXLINK_RETRY_MS * 8u;
	cxlink.seq_net_tx++;
	cxlink.net_tx_frames++;
	cxlink_flush();
	return true;
}

/*
 * Called by the NE2000 backend to pull one received frame.
 * Returns the frame length, or 0 when the queue is empty.
 */
int cxlink_net_poll_frame(uint8_t *buffer, size_t capacity)
{
	uint16_t length;

	if (cxlink.net_rx_queue.tail == cxlink.net_rx_queue.head)
		return 0;
	length = cxlink.net_rx_length[cxlink.net_rx_queue.tail];
	if (length > capacity)
		length = (uint16_t)capacity;
	memcpy(buffer, cxlink.net_rx_data[cxlink.net_rx_queue.tail], length);
	cxlink.net_rx_queue.tail =
		(uint16_t)((cxlink.net_rx_queue.tail + 1u) % CXLINK_NET_QUEUE);
	return (int)length;
}

/* ------------------------------------------------------------------ */
/* Wi-Fi provisioning                                                  */
/* ------------------------------------------------------------------ */
/*
 * The bridge has no console, no config file and no flash write: the credentials
 * for its uplink come up this link. That is the only place they can go - the
 * calculator is the user's own device and its config file is the only input the
 * user has - and it keeps a network password out of build configuration, which
 * is to say out of source control.
 *
 * The exchange is deliberately small and idempotent:
 *
 *   calculator                                    bridge
 *   ----------                                    ------
 *   NET_CONFIG{ssid,password}  ----------------->
 *                              <----------------- NET_CONFIG{flags,state}
 *   stop once CREDENTIALS is set      (also re-sent after a bridge restart)
 *
 * NET_CONFIG travels in the best-effort direction (no ACK): the message is
 * re-sent on a timer until confirmed, which costs less than an ACK exchange and
 * recovers a restarted bridge for free. The bridge ignores a pair it is already
 * using, so a retransmission never causes a re-association storm.
 */

/*
 * Bounded length of a NUL-terminated field, without strnlen() (not C99, and
 * this file is compiled for three toolchains). Never reads past `capacity`.
 */
static size_t cxlink_field_length(const uint8_t *field, size_t capacity)
{
	size_t length = 0;

	while (length < capacity && field[length])
		length++;
	return length;
}

static bool cxlink_field_text(const uint8_t *field, size_t length)
{
	size_t i;

	for (i = 0; i < length; i++) {
		/* Bytes below 0x20 cannot appear in an SSID and would break the
		 * INI file the credentials are typed into; DEL is equally
		 * meaningless here. Bytes >= 0x80 are left alone: an SSID is an
		 * octet string and may be UTF-8. */
		if (field[i] < 0x20u || field[i] == 0x7Fu)
			return false;
	}
	return true;
}

CxlinkWifiResult cxlink_wifi_check(const char *ssid, const char *password)
{
	size_t ssid_length;
	size_t password_length;

	/* NULL is "not supplied", which is the same as empty. */
	ssid_length = cxlink_field_length((const uint8_t *)(ssid ? ssid : ""),
					  CXLINK_WIFI_SSID_MAX);
	password_length = cxlink_field_length(
		(const uint8_t *)(password ? password : ""),
		CXLINK_WIFI_PASSWORD_MAX);

	if (!ssid_length)
		return password_length ? CXLINK_WIFI_PASSWORD_WITHOUT_SSID
				       : CXLINK_WIFI_NO_SSID;
	if (ssid_length > CXLINK_WIFI_SSID_MAX_LEN)
		return CXLINK_WIFI_SSID_TOO_LONG;
	/*
	 * An empty password is legal (open network). Between 1 and 7 characters is
	 * not: every WPA/WPA2/WPA3 passphrase is at least 8, so accepting it would
	 * only produce an association failure the user cannot interpret.
	 */
	if (password_length > CXLINK_WIFI_PASSWORD_MAX_LEN)
		return CXLINK_WIFI_PASSWORD_TOO_LONG;
	if (password_length && password_length < CXLINK_WIFI_PASSWORD_MIN_LEN)
		return CXLINK_WIFI_PASSWORD_TOO_SHORT;
	if (!cxlink_field_text((const uint8_t *)(ssid ? ssid : ""),
			       ssid_length) ||
	    !cxlink_field_text((const uint8_t *)(password ? password : ""),
			       password_length))
		return CXLINK_WIFI_BAD_CHARACTER;
	return CXLINK_WIFI_OK;
}

const char *cxlink_wifi_result_text(CxlinkWifiResult result)
{
	switch (result) {
	case CXLINK_WIFI_OK:
		return "accepted";
	case CXLINK_WIFI_NO_SSID:
		return "no ssid given";
	case CXLINK_WIFI_PASSWORD_WITHOUT_SSID:
		return "password given without an ssid";
	case CXLINK_WIFI_SSID_TOO_LONG:
		return "ssid is longer than 31 characters";
	case CXLINK_WIFI_PASSWORD_TOO_SHORT:
		return "password is shorter than 8 characters";
	case CXLINK_WIFI_PASSWORD_TOO_LONG:
		return "password is longer than 63 characters";
	case CXLINK_WIFI_BAD_CHARACTER:
		return "ssid or password contains a control character";
	default:
		break;
	}
	return "unknown";
}

/*
 * Put one NET_CONFIG frame on the wire.
 *
 * Only the credentials are filled in. mac, flags and state belong to the
 * bridge's report - the calculator has no business telling the bridge what its
 * own hardware address is, and a stale echo of the bridge's flags must never be
 * mistaken for the bridge itself.
 */
static void cxlink_net_config_send(void)
{
	CxlinkNetConfig config;
	uint8_t out[CXLINK_MAX_FRAME];
	size_t total;

	memset(&config, 0, sizeof(config));
	memcpy(config.ssid, cxlink.provision_ssid, sizeof(config.ssid));
	memcpy(config.password, cxlink.provision_password,
	       sizeof(config.password));
	total = cxlink_encode(CXLINK_MSG_NET_CONFIG, 0, cxlink.seq_net_config++,
			      (const uint8_t *)&config, sizeof(config), out,
			      sizeof(out));
	if (!total)
		return;
	if (cxlink_ring_write(&cxlink.tx_ring, out, total) == total) {
		cxlink.provision_sends++;
		cxlink_flush();
	}
}

CxlinkWifiResult cxlink_net_provision(const char *ssid, const char *password)
{
	CxlinkWifiResult result = cxlink_wifi_check(ssid, password);
	size_t ssid_length;
	size_t password_length;

	/* Whatever was pending is replaced, whether or not this pair is usable. */
	cxlink_net_provision_clear();
	if (result != CXLINK_WIFI_OK)
		return result;

	ssid_length = cxlink_field_length((const uint8_t *)ssid,
					  CXLINK_WIFI_SSID_MAX);
	password_length = cxlink_field_length(
		(const uint8_t *)(password ? password : ""),
		CXLINK_WIFI_PASSWORD_MAX);
	memcpy(cxlink.provision_ssid, ssid, ssid_length);
	if (password_length)
		memcpy(cxlink.provision_password, password, password_length);
	cxlink.provision_pending = true;
	cxlink.provision_confirmed = false;
	/* Send on the next poll the link is up for, not after a full retry period. */
	cxlink.provision_next_ms = 0;
	return result;
}

void cxlink_net_provision_clear(void)
{
	cxlink.provision_pending = false;
	cxlink.provision_confirmed = false;
	/*
	 * The password is wiped rather than merely forgotten. Nothing reads it
	 * after this, and a secret that is no longer needed should not stay in
	 * the heap for the rest of the session.
	 */
	memset(cxlink.provision_ssid, 0, sizeof(cxlink.provision_ssid));
	memset(cxlink.provision_password, 0, sizeof(cxlink.provision_password));
}

void cxlink_net_provision_status(CxlinkProvisionStatus *status)
{
	if (!status)
		return;
	status->pending = cxlink.provision_pending;
	status->confirmed = cxlink.provision_confirmed;
	status->flags = cxlink.net_flags;
	status->state = cxlink.net_state;
	status->sends = cxlink.provision_sends;
}

/* ------------------------------------------------------------------ */
/* audio plumbing                                                      */
/* ------------------------------------------------------------------ */

/*
 * The Sound Blaster feeds PCM into the shared mixer, which calls
 * cxlink_audio_write(). Audio blocks are queued as AUDIO_DATA frames with no
 * ACK: a late sample is useless, so the path is lossy by design and the guest
 * is never blocked.
 */
void cxlink_audio_configure(const CxlinkAudioConfig *config)
{
	if (!config)
		return;
	cxlink.audio = *config;
	if (!cxlink.audio.channels)
		cxlink.audio.channels = 1;
	if (!cxlink.audio.volume)
		cxlink.audio.volume = 256;
	cxlink.audio_configured = true;
	cxlink_send(CXLINK_MSG_AUDIO_CONFIG, CXLINK_FLAG_STREAM_END,
		    cxlink.seq_audio, (const uint8_t *)&cxlink.audio,
		    sizeof(cxlink.audio));
}

/*
 * Frame up mono 8-bit signed samples as AUDIO_DATA.
 *
 * This layer is rate-agnostic on purpose: it takes whatever it is given and
 * frames it. Rate conversion lives one level up in
 * cxlink_audio_write_mixer_block(), which is shared by the frontends so there
 * is exactly one implementation of the arithmetic to get right.
 */
void cxlink_audio_write(const int8_t *pcm, size_t sample_count)
{
	uint8_t block[CXLINK_AUDIO_BLOCK];
	size_t block_length = 0;
	size_t i;

	if (!cxlink.audio_configured || !pcm || !sample_count)
		return;
	if (cxlink.audio.flags & CXLINK_AUDIO_FLAG_MUTE)
		return;
	if (cxlink.audio.format != CXLINK_AUDIO_FMT_U8_S8)
		return;

	for (i = 0; i < sample_count; i++) {
		block[block_length++] = (uint8_t)pcm[i];
		if (block_length == sizeof(block)) {
			cxlink_send(CXLINK_MSG_AUDIO_DATA, 0,
				    cxlink.seq_audio++, block, block_length);
			cxlink.audio_frames_sent++;
			block_length = 0;
		}
	}
	if (block_length) {
		cxlink_send(CXLINK_MSG_AUDIO_DATA, CXLINK_FLAG_STREAM_END,
			    cxlink.seq_audio++, block, block_length);
		cxlink.audio_frames_sent++;
	}
}

void cxlink_audio_flush(void)
{
	cxlink_send(CXLINK_MSG_AUDIO_FLUSH, 0, cxlink.seq_audio, NULL, 0);
}

uint32_t cxlink_audio_rate(void)
{
	return cxlink.audio.sample_rate ? cxlink.audio.sample_rate
					: CXLINK_AUDIO_DEFAULT_RATE;
}

void cxlink_audio_resampler_init(CxlinkAudioResampler *resampler)
{
	if (!resampler)
		return;
	memset(resampler, 0, sizeof(*resampler));
}

size_t cxlink_audio_resampler_feed(CxlinkAudioResampler *resampler,
				   const int16_t *stereo, size_t frames, bool flush)
{
	uint32_t rate;
	size_t forwarded = 0;
	size_t i;

	if (!resampler)
		return 0;

	if (stereo && frames) {
		rate = cxlink_audio_rate();
		/* Never upsample: interpolating would cost more than it is worth
		 * on this link and cannot add information. */
		if (rate > CXLINK_MIXER_RATE)
			rate = CXLINK_MIXER_RATE;

		for (i = 0; i < frames; i++) {
			resampler->accum += rate;
			if (resampler->accum < CXLINK_MIXER_RATE)
				continue;
			resampler->accum -= CXLINK_MIXER_RATE;
			/*
			 * s16 stereo -> s8 mono. The shift is 9, not 8: the sum of
			 * two samples is in [-65536, 65534], so >>9 lands in
			 * [-128, 127] exactly and needs neither clipping nor a
			 * divide. Truncating rather than rounding is deliberate -- 8
			 * kHz on a 115200 baud link, not the resampler, is the
			 * quality limit.
			 */
			resampler->buffer[resampler->pending++] =
				(int8_t)((stereo[i * 2] +
					  stereo[i * 2 + 1]) >> 9);
			if (resampler->pending == CXLINK_AUDIO_BLOCK) {
				cxlink_audio_write(resampler->buffer,
						   resampler->pending);
				forwarded += resampler->pending;
				resampler->pending = 0;
			}
		}
	}

	if (flush && resampler->pending) {
		cxlink_audio_write(resampler->buffer, resampler->pending);
		forwarded += resampler->pending;
		resampler->pending = 0;
	}
	return forwarded;
}

/* ------------------------------------------------------------------ */
/* receive dispatch                                                    */
/* ------------------------------------------------------------------ */

static void cxlink_handle_frame(const CxlinkFrame *frame)
{
	switch (frame->type) {
	case CXLINK_MSG_HELLO:
		/* Arrival already marked the link up; reply with our protocol
		 * version so a mismatch surfaces early. */
		{
			CxlinkVersion version = { CXLINK_VERSION_MAJOR,
						  CXLINK_VERSION_MINOR,
						  { 'N', 'S', 'P', 'R' },
						  { 0, 0 } };

			cxlink_send(CXLINK_MSG_VERSION, 0, cxlink.seq_mgmt++,
				    (const uint8_t *)&version, sizeof(version));
		}
		break;

	case CXLINK_MSG_VERSION:
		if (frame->length >= 2 &&
		    (frame->payload[0] != CXLINK_VERSION_MAJOR))
			cxlink.link_errors++;
		break;

	case CXLINK_MSG_PING:
		cxlink_send(CXLINK_MSG_PONG, 0, frame->sequence,
			    frame->payload, frame->length);
		break;

	case CXLINK_MSG_PONG:
		break;

	case CXLINK_MSG_RESET:
		/* Far end restarted: drop the outstanding frame and re-handshake. */
		cxlink.pending.in_use = false;
		cxlink.link_up = false;
		cxlink_send(CXLINK_MSG_HELLO, 0, cxlink.seq_mgmt++, NULL, 0);
		break;

	case CXLINK_MSG_ACK:
		if (frame->length >= 4) {
			uint16_t acked = (uint16_t)(frame->payload[2] |
						    (frame->payload[3] << 8));

			if (cxlink.pending.in_use &&
			    acked == cxlink.pending.sequence)
				cxlink.pending.in_use = false;
		}
		break;

	case CXLINK_MSG_NACK:
		if (frame->length >= 4) {
			uint16_t acked = (uint16_t)(frame->payload[2] |
						    (frame->payload[3] << 8));

			/* Retry immediately rather than waiting out the timer. */
			if (cxlink.pending.in_use &&
			    acked == cxlink.pending.sequence)
				cxlink.pending_not_before_ms = 0;
		}
		break;

	case CXLINK_MSG_AUDIO_STATUS:
		if (frame->length >= sizeof(CxlinkAudioStatus)) {
			const CxlinkAudioStatus *status =
				(const CxlinkAudioStatus *)frame->payload;

			cxlink.audio_underruns = status->underruns;
			cxlink.audio_configured = true;
		}
		break;

	case CXLINK_MSG_NET_RX:
		if (frame->length && frame->length <= CXLINK_MAX_PAYLOAD) {
			if (frame->flags & CXLINK_FLAG_ACK_REQ)
				cxlink_send_ack(frame->type, frame->flags,
						frame->sequence, 0);
			cxlink_net_queue_push(frame);
		}
		break;

	case CXLINK_MSG_NET_CONFIG:
		if (frame->length >= sizeof(CxlinkNetConfig)) {
			const CxlinkNetConfig *config =
				(const CxlinkNetConfig *)frame->payload;

			cxlink.net_flags = config->flags;
			cxlink.net_state = config->state;
			/* Liveness is not this frame's business; see cxlink_poll(). */
			/*
			 * This is the acknowledgement for provisioning. A bridge that
			 * restarted reports "no credentials", which puts them back on
			 * the wire; that is the whole recovery mechanism, and it is why
			 * the state is re-armed from here rather than from a timer.
			 */
			if (config->flags & CXLINK_NET_FLAG_CREDENTIALS) {
				cxlink.provision_confirmed = true;
				cxlink.provision_pending = false;
			} else if (cxlink.provision_ssid[0]) {
				cxlink.provision_confirmed = false;
				cxlink.provision_pending = true;
				/* Re-provision now, not after the retry period. */
				cxlink.provision_next_ms = 0;
			}
		}
		break;

	case CXLINK_MSG_ERROR:
		cxlink.link_errors++;
		break;

	default:
		/* Unknown type: the peer may be newer. Ignore rather than reset. */
		cxlink.link_errors++;
		break;
	}
}

/* ------------------------------------------------------------------ */
/* periodic service                                                    */
/* ------------------------------------------------------------------ */

/*
 * Drive the bridge. Called once per emulation batch from the frontend, not
 * from the guest-execution inner loop, so its cost is amortised over a whole
 * batch of x86 instructions.
 */
void cxlink_poll(void)
{
	const CxlinkFrame *frame = NULL;
	uint8_t byte;
	uint32_t now;

	/* 1. Pull bytes from the transport and decode. */
	while (cxlink.hal.read &&
	       cxlink.hal.read(cxlink.hal_context, &byte, 1) == 1) {
		if (cxlink_decoder_push(&cxlink.decoder, byte, &frame) && frame) {
			/*
			 * A frame that decoded and passed its CRC can only have come
			 * from a live bridge, whatever it says about its own uplink.
			 * RESET is the one handler that may clear this again.
			 */
			cxlink.link_up = true;
			cxlink.mgmt_last_rx_ms = cxlink_now_ms();
			cxlink_handle_frame(frame);
		}
	}

	/* 2. Service the reliable channel's retransmit timer. */
	now = cxlink_now_ms();
	if (cxlink.pending.in_use && now >= cxlink.pending_not_before_ms) {
		uint8_t out[CXLINK_MAX_FRAME];
		size_t total;

		if (cxlink.pending.retries >= CXLINK_RETRY_MAX ||
		    now > cxlink.pending_deadline_ms) {
			/* Peer is not answering; treat the link as down. */
			cxlink.pending.in_use = false;
			cxlink.link_errors++;
			cxlink.link_up = false;
		} else {
			total = cxlink_encode(CXLINK_MSG_NET_TX,
					      CXLINK_FLAG_ACK_REQ |
						      CXLINK_FLAG_RETRY,
					      cxlink.pending.sequence,
					      cxlink.pending.data,
					      cxlink.pending.length, out,
					      sizeof(out));
			if (total && cxlink_ring_write(&cxlink.tx_ring, out,
						       total) == total) {
				cxlink.pending.retries++;
				cxlink.pending_not_before_ms =
					now + CXLINK_RETRY_MS;
				cxlink_flush();
			}
		}
	}

	/* 3. Keepalive so a silent ESTOP or unplugged dock is noticed. */
	if (cxlink.link_up &&
	    now - cxlink.mgmt_last_rx_ms > 2000u &&
	    now - cxlink.probe_last_ms > 2000u) {
		cxlink_send(CXLINK_MSG_PING, 0, cxlink.seq_mgmt++, NULL, 0);
		cxlink.probe_last_ms = now;
	}
	/*
	 * A bridge that has said nothing at all for four report intervals is gone.
	 * Saying so is what stops the guest pushing frames into a void and lets
	 * the frontend report "link lost" instead of "link up, nothing moving".
	 */
	if (cxlink.link_up &&
	    now - cxlink.mgmt_last_rx_ms > CXLINK_BRIDGE_TIMEOUT_MS) {
		cxlink.link_up = false;
		cxlink.link_errors++;
	}

	/*
	 * 4. Keep the bridge provisioned. Credentials are re-sent until the
	 * bridge reports holding them, so a lost frame - or a bridge that was
	 * unplugged and restarted, which forgets them - is recovered here rather
	 * than in the frontend.
	 */
	if (cxlink.provision_pending && cxlink.link_up &&
	    (int32_t)(now - cxlink.provision_next_ms) >= 0) {
		cxlink_net_config_send();
		cxlink.provision_next_ms = now + CXLINK_PROVISION_RETRY_MS;
	}

	/* 5. Push whatever is buffered. */
	cxlink_flush();
}

/* ------------------------------------------------------------------ */
/* bring-up and status                                                 */
/* ------------------------------------------------------------------ */

void cxlink_init(const CxlinkHal *hal, void *hal_context)
{
	CxlinkAudioConfig audio;

	memset(&cxlink, 0, sizeof(cxlink));
	cxlink_decoder_init(&cxlink.decoder);
	if (hal)
		cxlink.hal = *hal;
	cxlink.hal_context = hal_context;

	/* Start the handshake so the far end knows we exist. */
	cxlink_send(CXLINK_MSG_HELLO, 0, cxlink.seq_mgmt++, NULL, 0);

	/*
	 * Publish the audio format through the normal configure path rather than
	 * poking the struct directly. That both tells the ESP32 what to expect
	 * and arms audio_configured, so the mixer can start handing us blocks
	 * immediately instead of dropping everything until the ESP32 replies.
	 */
	memset(&audio, 0, sizeof(audio));
	audio.sample_rate = CXLINK_AUDIO_DEFAULT_RATE;
	audio.channels = 1;
	audio.format = CXLINK_AUDIO_FMT_U8_S8;
	audio.volume = 256;
	cxlink_audio_configure(&audio);
}

bool cxlink_link_up(void)
{
	return cxlink.link_up;
}

void cxlink_get_status(CxlinkNetStatus *net, CxlinkAudioStatus *audio)
{
	if (net) {
		net->tx_frames = cxlink.net_tx_frames;
		net->rx_frames = cxlink.net_rx_frames;
		net->tx_dropped = (uint16_t)cxlink.net_tx_dropped;
		net->link_errors = (uint16_t)cxlink.link_errors;
	}
	if (audio) {
		audio->queued_frames = 0;
		audio->underruns = cxlink.audio_underruns;
		audio->overruns = cxlink.audio_dropped;
		audio->link_errors = (uint16_t)cxlink.link_errors;
	}
}

/* ------------------------------------------------------------------ */
/* Transport HAL: SoC UART on the CX dock connector                    */
/* ------------------------------------------------------------------ */
/*
 * IMPORTANT - VERIFY ON HARDWARE BEFORE TRUSTING THIS.
 *
 * Ndless exposes no UART API. The dock connector's Tx/Rx/GND pins and the
 * 115200 8N1 line settings are documented (Hackspire, "Connector J01"), but the
 * SoC UART register base for the CX ASIC is not part of the Ndless API and has
 * not been confirmed by the author of this port. The defaults below follow the
 * register layout of the classic Nspire's PrimeCell-style UART; if the link
 * does not come up on real hardware, the two things to check are
 * WINSPIRE_CXLINK_UART_BASE and whether the dock connector is populated at all
 * on that board revision (it was removed on some).
 *
 * Until then this backend is opt-in: build with -DWINSPIRE_CXLINK_UART to
 * enable it. Without that flag cxlink runs with no transport, which keeps the
 * rest of the system testable and makes the failure mode obvious (no frames
 * move) rather than a hang.
 */
#ifdef BUILD_NSPIRE
#ifdef WINSPIRE_CXLINK_UART

#ifndef WINSPIRE_CXLINK_UART_BASE
#define WINSPIRE_CXLINK_UART_BASE 0x90040000u
#endif
#define UART_DR 0x00u   /* data register */
#define UART_FR 0x18u   /* flag register: bit 5 = TX FIFO full */
#define UART_FR_TXFF (1u << 5)

static size_t cxlink_uart_write(void *context, const uint8_t *data,
				size_t length)
{
	volatile uint32_t *base = (volatile uint32_t *)WINSPIRE_CXLINK_UART_BASE;
	size_t written = 0;

	(void)context;
	while (written < length) {
		if (*(base + (UART_FR / 4)) & UART_FR_TXFF)
			break;
		*(base + (UART_DR / 4)) = data[written++];
	}
	return written;
}

static size_t cxlink_uart_read(void *context, uint8_t *data, size_t capacity)
{
	volatile uint32_t *base = (volatile uint32_t *)WINSPIRE_CXLINK_UART_BASE;
	size_t read = 0;

	(void)context;
	while (read < capacity) {
		uint32_t flags = *(base + (UART_FR / 4));

		if (flags & (1u << 4)) /* RX FIFO empty */
			break;
		data[read++] = (uint8_t)(*(base + (UART_DR / 4)) & 0xFFu);
	}
	return read;
}

static size_t cxlink_uart_pending(void *context)
{
	(void)context;
	return 0;
}

void cxlink_start_uart(void)
{
	CxlinkHal hal;

	hal.write = cxlink_uart_write;
	hal.read = cxlink_uart_read;
	hal.pending = cxlink_uart_pending;
	cxlink_init(&hal, NULL);
}

#endif /* WINSPIRE_CXLINK_UART */
#endif /* BUILD_NSPIRE */

void cxlink_start_null(void)
{
	cxlink_init(NULL, NULL);
}

/*
 * Pick whatever transport this build actually has. Keeping the choice here
 * rather than in the frontend means the frontend has one call site and no
 * #ifdef of its own.
 */
void cxlink_start_default(void)
{
#if defined(BUILD_NSPIRE) && defined(WINSPIRE_CXLINK_UART)
	cxlink_start_uart();
#else
	cxlink_start_null();
#endif
}

/* ------------------------------------------------------------------ */
/* self test                                                           */
/* ------------------------------------------------------------------ */
#ifdef CXLINK_ENABLE_SELFTEST

/*
 * An in-memory transport that never echoes.
 *
 * The calculator's outbound bytes land in `capture` for the test to inspect,
 * while `wire` only carries what the test injects as if it came from the
 * ESP32. That separation matters: a loopback that echoed the calculator's own
 * NET_TX back would exercise a code path that never runs against the real
 * peer, and it would hide the fact that NET_TX is outbound-only.
 */
#define CXLINK_TEST_LINK_SIZE 8192u
#define CXLINK_TEST_MAX_FRAMES 8u

typedef struct {
	uint8_t wire[CXLINK_TEST_LINK_SIZE];
	size_t wire_head;
	size_t wire_tail;
	uint8_t capture[CXLINK_TEST_LINK_SIZE];
	size_t capture_length;
} CxlinkTestLink;

typedef struct {
	CxlinkFrame frames[CXLINK_TEST_MAX_FRAMES];
	unsigned int count;
} CxlinkTestLog;

static size_t cxlink_test_write(void *context, const uint8_t *data,
				size_t length)
{
	CxlinkTestLink *link = context;
	size_t room = sizeof(link->capture) - link->capture_length;

	if (length > room)
		length = room;
	memcpy(link->capture + link->capture_length, data, length);
	link->capture_length += length;
	return length;
}

static size_t cxlink_test_read(void *context, uint8_t *data, size_t capacity)
{
	CxlinkTestLink *link = context;
	size_t available = link->wire_head - link->wire_tail;

	if (available > capacity)
		available = capacity;
	memcpy(data, link->wire + link->wire_tail, available);
	link->wire_tail += available;
	return available;
}

static size_t cxlink_test_pending(void *context)
{
	(void)context;
	return 0;
}

static bool cxlink_test_inject_raw(CxlinkTestLink *link, const uint8_t *data,
				   size_t length)
{
	if (link->wire_head + length > sizeof(link->wire))
		return false;
	memcpy(link->wire + link->wire_head, data, length);
	link->wire_head += length;
	return true;
}

static bool cxlink_test_inject(CxlinkTestLink *link, uint8_t type,
			       uint8_t flags, uint16_t sequence,
			       const uint8_t *payload, size_t length)
{
	uint8_t encoded[CXLINK_MAX_FRAME];
	size_t total = cxlink_encode(type, flags, sequence, payload, length,
				     encoded, sizeof(encoded));

	if (!total)
		return false;
	return cxlink_test_inject_raw(link, encoded, total);
}

/* Decode and clear everything the calculator has sent since the last drain. */
static void cxlink_test_drain(CxlinkTestLink *link, CxlinkTestLog *log)
{
	CxlinkDecoder decoder;
	const CxlinkFrame *frame = NULL;
	size_t i;

	cxlink_decoder_init(&decoder);
	log->count = 0;
	for (i = 0; i < link->capture_length; i++) {
		if (cxlink_decoder_push(&decoder, link->capture[i], &frame) &&
		    frame) {
			if (log->count < CXLINK_TEST_MAX_FRAMES)
				log->frames[log->count++] = *frame;
		}
	}
	link->capture_length = 0;
}

static bool cxlink_test_found(const CxlinkTestLog *log, uint8_t type)
{
	unsigned int i;

	for (i = 0; i < log->count; i++)
		if (log->frames[i].type == type)
			return true;
	return false;
}

int cxlink_selftest_failure_line;

/*
 * Set aside from assert(): the host build runs this without NDEBUG discipline,
 * and the calculator build must not pull in assert machinery. The failing line
 * is recorded rather than printed so this file stays free of stdio.
 */
#define CXLINK_CHECK(cond) do { \
	if (!(cond)) { \
		cxlink_selftest_failure_line = __LINE__; \
		return false; \
	} \
} while (0)

bool cxlink_selftest(void)
{
	static CxlinkTestLink link;
	static CxlinkTestLog log;
	static const size_t codec_lengths[] = { 0, 1, 2, 7, 64, 128, 1024,
						CXLINK_MAX_PAYLOAD };
	CxlinkHal hal;
	CxlinkDecoder decoder;
	const CxlinkFrame *decoded = NULL;
	uint8_t payload[CXLINK_MAX_PAYLOAD];
	uint8_t scratch[CXLINK_MAX_FRAME];
	uint8_t temp[CXLINK_MAX_FRAME];
	uint8_t ack[4];
	int8_t samples[300];
	CxlinkNetStatus net_status;
	CxlinkAudioStatus audio_status;
	unsigned int k;
	size_t i, total, n;
	int nframe;

	cxlink_selftest_failure_line = 0;

	/* A ramp with both signs, built once and reused by the audio checks. */
	for (i = 0; i < 300; i++)
		samples[i] = (int8_t)((int)i - 128);

	/* ---- 1. codec round trip over the payload range ---- */
	for (k = 0; k < sizeof(codec_lengths) / sizeof(codec_lengths[0]); k++) {
		size_t length = codec_lengths[k];

		for (i = 0; i < length; i++)
			payload[i] = (uint8_t)(i * 31u + 7u);
		total = cxlink_encode(CXLINK_MSG_NET_RX, CXLINK_FLAG_ACK_REQ,
				      0x1234u, payload, length, scratch,
				      sizeof(scratch));
		CXLINK_CHECK(total == length + CXLINK_FRAME_OVERHEAD);

		cxlink_decoder_init(&decoder);
		nframe = 0;
		for (i = 0; i < total; i++) {
			if (cxlink_decoder_push(&decoder, scratch[i], &decoded))
				nframe++;
		}
		CXLINK_CHECK(nframe == 1 && decoded != NULL);
		CXLINK_CHECK(decoded->type == CXLINK_MSG_NET_RX);
		CXLINK_CHECK(decoded->flags == CXLINK_FLAG_ACK_REQ);
		CXLINK_CHECK(decoded->sequence == 0x1234u);
		CXLINK_CHECK(decoded->length == length);
		CXLINK_CHECK(memcmp(decoded->payload, payload, length) == 0);

		/* A mangled payload must be rejected, not delivered. */
		if (length) {
			uint8_t saved = scratch[CXLINK_HEADER_SIZE];

			scratch[CXLINK_HEADER_SIZE] ^= 0xFFu;
			cxlink_decoder_init(&decoder);
			nframe = 0;
			for (i = 0; i < total; i++)
				if (cxlink_decoder_push(&decoder, scratch[i],
							&decoded))
					nframe++;
			CXLINK_CHECK(nframe == 0);
			CXLINK_CHECK(decoder.frames_dropped_crc == 1);
			scratch[CXLINK_HEADER_SIZE] = saved;
		}

		/* Too-small output buffers must be refused, not truncated. */
		CXLINK_CHECK(cxlink_encode(CXLINK_MSG_NET_RX, 0, 0, payload,
					   length, scratch,
					   total - 1) == 0);
	}

	/* An impossible length field must resynchronise instead of overrunning. */
	{
		static const uint8_t bad[] = { CXLINK_SYNC0, CXLINK_SYNC1,
					       CXLINK_MSG_NET_RX, 0,
					       0, 0, 0xFF, 0xFF };

		cxlink_decoder_init(&decoder);
		nframe = 0;
		for (i = 0; i < sizeof(bad); i++)
			if (cxlink_decoder_push(&decoder, bad[i], &decoded))
				nframe++;
		CXLINK_CHECK(nframe == 0);
		CXLINK_CHECK(decoder.frames_dropped_format == 1);
	}

	/* ---- 2. bring-up ---- */
	memset(&link, 0, sizeof(link));
	hal.write = cxlink_test_write;
	hal.read = cxlink_test_read;
	hal.pending = cxlink_test_pending;
	cxlink_init(&hal, &link);

	cxlink_test_drain(&link, &log);
	/* HELLO to announce us, then the audio format we want to receive at. */
	CXLINK_CHECK(log.count == 2);
	CXLINK_CHECK(log.frames[0].type == CXLINK_MSG_HELLO);
	CXLINK_CHECK(log.frames[0].length == 0);
	CXLINK_CHECK(log.frames[1].type == CXLINK_MSG_AUDIO_CONFIG);
	CXLINK_CHECK(log.frames[1].length == sizeof(CxlinkAudioConfig));
	{
		CxlinkAudioConfig cfg;

		memcpy(&cfg, log.frames[1].payload, sizeof(cfg));
		CXLINK_CHECK(cfg.sample_rate == CXLINK_AUDIO_DEFAULT_RATE);
		CXLINK_CHECK(cfg.channels == 1);
		CXLINK_CHECK(cfg.format == CXLINK_AUDIO_FMT_U8_S8);
			CXLINK_CHECK(cxlink_audio_rate() == cfg.sample_rate);
	}

	/*
	 * The format is armed by init rather than by the first ESP32 reply, so
	 * the mixer can push audio immediately: this must produce a frame even
	 * though the handshake has not completed yet.
	 */
	cxlink_audio_write(samples, 4);
	cxlink_test_drain(&link, &log);
	CXLINK_CHECK(log.count == 1);
	CXLINK_CHECK(log.frames[0].type == CXLINK_MSG_AUDIO_DATA);
	CXLINK_CHECK(log.frames[0].length == 4);
	CXLINK_CHECK(!cxlink_link_up());

	/* The ESP32 answers the handshake. */
	CXLINK_CHECK(cxlink_test_inject(&link, CXLINK_MSG_HELLO, 0, 0, NULL, 0));
	cxlink_poll();
	CXLINK_CHECK(cxlink_link_up());
	cxlink_test_drain(&link, &log);
	CXLINK_CHECK(cxlink_test_found(&log, CXLINK_MSG_VERSION));

	/* ---- 3. inbound Ethernet frame ---- */
	for (i = 0; i < 200; i++)
		payload[i] = (uint8_t)(i * 7u + 3u);
	CXLINK_CHECK(cxlink_test_inject(&link, CXLINK_MSG_NET_RX,
					CXLINK_FLAG_ACK_REQ, 42, payload, 200));
	cxlink_poll();
	cxlink_test_drain(&link, &log);
	/* Exactly one ACK, echoing the acked type and sequence. */
	CXLINK_CHECK(log.count == 1);
	CXLINK_CHECK(log.frames[0].type == CXLINK_MSG_ACK);
	CXLINK_CHECK(log.frames[0].length == 4);
	CXLINK_CHECK(log.frames[0].payload[0] == CXLINK_MSG_NET_RX);
	CXLINK_CHECK(log.frames[0].payload[2] == (uint8_t)(42 & 0xFF));
	CXLINK_CHECK(log.frames[0].payload[3] == (uint8_t)(42 >> 8));

	n = (size_t)cxlink_net_poll_frame(scratch, sizeof(scratch));
	CXLINK_CHECK(n == 200);
	CXLINK_CHECK(memcmp(scratch, payload, 200) == 0);
	CXLINK_CHECK(cxlink_net_poll_frame(scratch, sizeof(scratch)) == 0);

	/* ---- 4. outbound Ethernet frame is reliable and exclusive ---- */
	for (i = 0; i < 512; i++)
		payload[i] = (uint8_t)(255u - (i & 0xFFu));
	CXLINK_CHECK(cxlink_net_send_frame(payload, 512));
	cxlink_test_drain(&link, &log);
	CXLINK_CHECK(log.count == 1);
	CXLINK_CHECK(log.frames[0].type == CXLINK_MSG_NET_TX);
	CXLINK_CHECK(log.frames[0].flags & CXLINK_FLAG_ACK_REQ);
	CXLINK_CHECK(log.frames[0].sequence == 0);
	CXLINK_CHECK(log.frames[0].length == 512);
	CXLINK_CHECK(memcmp(log.frames[0].payload, payload, 512) == 0);
	/* One outstanding reliable frame at a time. */
	CXLINK_CHECK(!cxlink_net_send_frame(payload, 512));

	ack[0] = CXLINK_MSG_NET_TX;
	ack[1] = CXLINK_FLAG_ACK_REQ;
	ack[2] = (uint8_t)(log.frames[0].sequence & 0xFF);
	ack[3] = (uint8_t)(log.frames[0].sequence >> 8);
	CXLINK_CHECK(cxlink_test_inject(&link, CXLINK_MSG_ACK, 0, 9, ack,
					4));
	cxlink_poll();
	CXLINK_CHECK(cxlink_net_send_frame(payload, 512)); /* window reopened */
	cxlink_test_drain(&link, &log);

	/* ---- 5. audio routing (the point of the whole exercise) ---- */
	cxlink_audio_write(samples, 300);
	cxlink_test_drain(&link, &log);
	/* 300 samples at a 128-sample block size: 128 + 128 + 44. */
	CXLINK_CHECK(log.count == 3);
	CXLINK_CHECK(log.frames[0].type == CXLINK_MSG_AUDIO_DATA);
	CXLINK_CHECK(log.frames[0].length == 128);
	CXLINK_CHECK(log.frames[1].length == 128);
	CXLINK_CHECK(log.frames[2].length == 44);
	CXLINK_CHECK(log.frames[2].flags & CXLINK_FLAG_STREAM_END);
	/* Sequences advance per block and per stream. */
	CXLINK_CHECK(log.frames[0].sequence + 1 == log.frames[1].sequence);
	CXLINK_CHECK(log.frames[1].sequence + 1 == log.frames[2].sequence);
	/* Samples survive byte-for-byte. */
	for (i = 0; i < 300; i++) {
		const CxlinkFrame *f = &log.frames[i / 128];

		CXLINK_CHECK(f->payload[i % 128] ==
			     (uint8_t)samples[i]);
	}

	/* Mute must silence the path without breaking framing. */
	cxlink_test_drain(&link, &log);
	{
		CxlinkAudioConfig muted;

		memset(&muted, 0, sizeof(muted));
		muted.sample_rate = CXLINK_AUDIO_DEFAULT_RATE;
		muted.channels = 1;
		muted.flags = CXLINK_AUDIO_FLAG_MUTE;
		cxlink_audio_configure(&muted);
		cxlink_test_drain(&link, &log);
		cxlink_audio_write(samples, 300);
		cxlink_test_drain(&link, &log);
		CXLINK_CHECK(log.count == 0);
	}

	/* ---- 6. audio status from the far end ---- */
	{
		CxlinkAudioStatus reported;

		memset(&reported, 0, sizeof(reported));
		reported.underruns = 5;
		reported.overruns = 2;
		CXLINK_CHECK(cxlink_test_inject(&link, CXLINK_MSG_AUDIO_STATUS,
						0, 3,
						(const uint8_t *)&reported,
						sizeof(reported)));
		cxlink_poll();
		cxlink_get_status(&net_status, &audio_status);
		CXLINK_CHECK(audio_status.underruns == 5);
	}

	/* ---- 7. corruption is dropped and the stream recovers ---- */
	cxlink_get_status(&net_status, NULL);
	{
		uint32_t rx_before = net_status.rx_frames;

		total = cxlink_encode(CXLINK_MSG_NET_RX, CXLINK_FLAG_ACK_REQ, 43,
				      payload, 64, temp, sizeof(temp));
		CXLINK_CHECK(total != 0);
		temp[total - 1] ^= 0xFFu; /* corrupt the CRC */
		CXLINK_CHECK(cxlink_test_inject_raw(&link, temp, total));
		cxlink_poll();
		cxlink_get_status(&net_status, NULL);
		CXLINK_CHECK(net_status.rx_frames == rx_before);
		CXLINK_CHECK(cxlink_net_poll_frame(scratch, sizeof(scratch)) == 0);

		/* A well-formed frame immediately after must still get through. */
		CXLINK_CHECK(cxlink_test_inject(&link, CXLINK_MSG_NET_RX,
						CXLINK_FLAG_ACK_REQ, 44,
						payload, 200));
		cxlink_poll();
		CXLINK_CHECK(cxlink_net_poll_frame(scratch,
						   sizeof(scratch)) == 200);
	}

	/* ---- 8. unknown message types, and RESET re-handshakes ---- */
	/* Clear the ACK step 7 produced so the RESET drain can be exact. */
	cxlink_test_drain(&link, &log);
	CXLINK_CHECK(cxlink_test_inject(&link, 0x7e, 0, 5, NULL, 0));
	cxlink_poll();
	CXLINK_CHECK(cxlink_link_up()); /* forward compatible, no reset */

	CXLINK_CHECK(cxlink_test_inject(&link, CXLINK_MSG_RESET, 0, 6, NULL, 0));
	cxlink_poll();
	CXLINK_CHECK(!cxlink_link_up());
	cxlink_test_drain(&link, &log);
	CXLINK_CHECK(log.count == 1);
	CXLINK_CHECK(log.frames[0].type == CXLINK_MSG_HELLO);

	/* ---- 9. mixer block -> link samples ---- */
	/* Undo step 5's mute so the conversion path is live again. */
	{
		CxlinkAudioConfig unmuted;

		memset(&unmuted, 0, sizeof(unmuted));
		unmuted.sample_rate = CXLINK_AUDIO_DEFAULT_RATE;
		unmuted.channels = 1;
		unmuted.volume = 256;
		cxlink_audio_configure(&unmuted);
		cxlink_test_drain(&link, &log);
	}
	{
		/*
		 * Both s16 extremes must survive the >>9 exactly. This is the one
		 * place the claim in the comment ('no clipping needed') is checked
		 * rather than asserted.
		 */
		static const int16_t extreme[2] = { 32767, -32768 };
		static const int8_t expected[2] = { 127, -128 };
		CxlinkAudioResampler resampler;
		size_t e, produced;

		for (e = 0; e < 2; e++) {
			int16_t stereo[8 * 2];

			for (i = 0; i < 8; i++)
				stereo[i * 2] = stereo[i * 2 + 1] = extreme[e];
			cxlink_audio_resampler_init(&resampler);
			cxlink_test_drain(&link, &log);
			/* 8 frames at 44100 Hz is 1.45 samples at 8000 Hz. */
			produced = cxlink_audio_resampler_feed(&resampler, stereo,
							       8, true);
			CXLINK_CHECK(produced == 1);
			cxlink_test_drain(&link, &log);
			CXLINK_CHECK(log.count == 1);
			CXLINK_CHECK(log.frames[0].type == CXLINK_MSG_AUDIO_DATA);
			CXLINK_CHECK(log.frames[0].length == 1);
			CXLINK_CHECK((int8_t)log.frames[0].payload[0] ==
				     expected[e]);
		}
	}
	{
		/*
		 * Partial blocks must be held, not framed, and the fractional sample
		 * position must carry. Four 200-frame blocks are 145 samples at
		 * 8000 Hz, so nothing leaves until the fourth, which crosses a whole
		 * 128-sample block and leaves 17 held.
		 */
		CxlinkAudioResampler resampler;
		int16_t stereo[200 * 2];
		size_t produced;
		int k;

		memset(stereo, 0, sizeof(stereo));
		cxlink_audio_resampler_init(&resampler);
		cxlink_test_drain(&link, &log);
		for (k = 0; k < 3; k++) {
			produced = cxlink_audio_resampler_feed(&resampler, stereo,
							       200, false);
			/* 108 samples held, still under one block. */
			CXLINK_CHECK(produced == 0);
		}
		CXLINK_CHECK(resampler.pending == 108);
		produced = cxlink_audio_resampler_feed(&resampler, stereo, 200, false);
		/* 145 samples now, so exactly one full block leaves. */
		CXLINK_CHECK(produced == 128);
		CXLINK_CHECK(resampler.pending == 17);
		CXLINK_CHECK(resampler.accum ==
			     800u * CXLINK_AUDIO_DEFAULT_RATE -
			     145u * CXLINK_MIXER_RATE);
		cxlink_test_drain(&link, &log);
		CXLINK_CHECK(log.count == 1);
		CXLINK_CHECK(log.frames[0].length == 128);

		/* Flushing with no input drains what was held. */
		cxlink_test_drain(&link, &log);
		produced = cxlink_audio_resampler_feed(&resampler, NULL, 0, true);
		CXLINK_CHECK(produced == 17);
		CXLINK_CHECK(resampler.pending == 0);
		cxlink_test_drain(&link, &log);
		CXLINK_CHECK(log.count == 1);
		CXLINK_CHECK(log.frames[0].length == 17);
		CXLINK_CHECK(log.frames[0].flags & CXLINK_FLAG_STREAM_END);

		/* A null resampler must be harmless, not a crash. */
		CXLINK_CHECK(cxlink_audio_resampler_feed(NULL, stereo, 200, true) == 0);
		cxlink_audio_resampler_init(NULL);
	}

	/* ---- 10. Wi-Fi provisioning ---- */
	/*
	 * Bounded fields first: the same validator runs on both ends, so a pair
	 * accepted here must never be one the bridge has to refuse.
	 */
	{
		static char long_ssid[CXLINK_WIFI_SSID_MAX];
		static char long_password[CXLINK_WIFI_PASSWORD_MAX];
		static char max_ssid[CXLINK_WIFI_SSID_MAX];
		static char max_password[CXLINK_WIFI_PASSWORD_MAX];

		/* The longest pair ESP-IDF's own fields can hold, terminator
		 * included: 31 characters of SSID, 63 of password. */
		memset(max_ssid, 'S', sizeof(max_ssid));
		max_ssid[CXLINK_WIFI_SSID_MAX_LEN] = '\0';
		memset(max_password, 'P', sizeof(max_password));
		max_password[CXLINK_WIFI_PASSWORD_MAX_LEN] = '\0';
		/* One character more than each field can hold. */
		memset(long_ssid, 'S', sizeof(long_ssid));
		long_ssid[CXLINK_WIFI_SSID_MAX_LEN + 1u] = '\0';
		memset(long_password, 'P', sizeof(long_password));
		long_password[CXLINK_WIFI_PASSWORD_MAX_LEN + 1u] = '\0';

		CXLINK_CHECK(cxlink_wifi_check("HomeNet", "supersecret123") ==
			     CXLINK_WIFI_OK);
		/* An open network is a network. */
		CXLINK_CHECK(cxlink_wifi_check("HomeNet", "") ==
			     CXLINK_WIFI_OK);
		/* The largest pair ESP-IDF can hold, terminator included. */
		CXLINK_CHECK(cxlink_wifi_check(max_ssid, max_password) ==
			     CXLINK_WIFI_OK);
		CXLINK_CHECK(cxlink_wifi_check("", "") == CXLINK_WIFI_NO_SSID);
		CXLINK_CHECK(cxlink_wifi_check(NULL, NULL) == CXLINK_WIFI_NO_SSID);
		CXLINK_CHECK(cxlink_wifi_check("", "supersecret123") ==
			     CXLINK_WIFI_PASSWORD_WITHOUT_SSID);
		CXLINK_CHECK(cxlink_wifi_check(long_ssid, "supersecret123") ==
			     CXLINK_WIFI_SSID_TOO_LONG);
		CXLINK_CHECK(cxlink_wifi_check("HomeNet", long_password) ==
			     CXLINK_WIFI_PASSWORD_TOO_LONG);
		/* 1..7 characters can never authenticate: refuse, do not try. */
		CXLINK_CHECK(cxlink_wifi_check("HomeNet", "short") ==
			     CXLINK_WIFI_PASSWORD_TOO_SHORT);
		CXLINK_CHECK(cxlink_wifi_check("HomeNet", "1234567") ==
			     CXLINK_WIFI_PASSWORD_TOO_SHORT);
		CXLINK_CHECK(cxlink_wifi_check("HomeNet", "12345678") ==
			     CXLINK_WIFI_OK);
		/* A control character would also corrupt the INI file entry. */
		CXLINK_CHECK(cxlink_wifi_check("Home\nNet", "supersecret123") ==
			     CXLINK_WIFI_BAD_CHARACTER);
		CXLINK_CHECK(cxlink_wifi_check("HomeNet", "secret\t123") ==
			     CXLINK_WIFI_BAD_CHARACTER);
		/* Every result has a sentence, including the ones above. */
		CXLINK_CHECK(cxlink_wifi_result_text(CXLINK_WIFI_OK)[0] != '\0');
		CXLINK_CHECK(cxlink_wifi_result_text(
				     CXLINK_WIFI_PASSWORD_TOO_SHORT)[0] != '\0');
		CXLINK_CHECK(cxlink_wifi_result_text(
				     CXLINK_WIFI_BAD_CHARACTER)[0] != '\0');
	}

	/* Bring the link back up: step 8's RESET took it down. */
	CXLINK_CHECK(cxlink_test_inject(&link, CXLINK_MSG_HELLO, 0, 100, NULL, 0));
	cxlink_poll();
	CXLINK_CHECK(cxlink_link_up());
	cxlink_test_drain(&link, &log);

	{
		CxlinkNetConfig sent;
		CxlinkProvisionStatus prov;

		CXLINK_CHECK(cxlink_net_provision("BridgeTest",
						  "supersecret123") ==
					 CXLINK_WIFI_OK);
		cxlink_poll();
		cxlink_test_drain(&link, &log);
		/* Exactly one credential frame, and nothing else. */
		CXLINK_CHECK(log.count == 1);
		CXLINK_CHECK(log.frames[0].type == CXLINK_MSG_NET_CONFIG);
		CXLINK_CHECK(log.frames[0].length == sizeof(CxlinkNetConfig));
		memcpy(&sent, log.frames[0].payload, sizeof(sent));
		CXLINK_CHECK(!strcmp((const char *)sent.ssid, "BridgeTest"));
		CXLINK_CHECK(!strcmp((const char *)sent.password,
				     "supersecret123"));
		/*
		 * The bridge's own report fields are left alone: a stale echo of
		 * them must never be mistaken for the bridge itself.
		 */
		CXLINK_CHECK(sent.flags == 0 && sent.state == 0);
		CXLINK_CHECK(sent.mac[0] == 0 && sent.mac[5] == 0);

		cxlink_net_provision_status(&prov);
		CXLINK_CHECK(prov.pending && !prov.confirmed);
		CXLINK_CHECK(prov.sends == 1);

		/* Not again until the retry period has passed. */
		cxlink_poll();
		cxlink_test_drain(&link, &log);
		CXLINK_CHECK(log.count == 0);

		/* The bridge confirms it holds them, and is connected. */
		{
			CxlinkNetConfig report;

			memset(&report, 0, sizeof(report));
			report.flags = CXLINK_NET_FLAG_UPLINK_UP |
				       CXLINK_NET_FLAG_CREDENTIALS;
			report.state = CXLINK_NET_STATE_CONNECTED;
			CXLINK_CHECK(cxlink_test_inject(&link, CXLINK_MSG_NET_CONFIG,
							0, 101,
							(const uint8_t *)&report,
							sizeof(report)));
		}
		cxlink_poll();
		cxlink_test_drain(&link, &log);
		CXLINK_CHECK(log.count == 0);
		cxlink_net_provision_status(&prov);
		CXLINK_CHECK(!prov.pending && prov.confirmed);
		CXLINK_CHECK(prov.state == CXLINK_NET_STATE_CONNECTED);
		CXLINK_CHECK(prov.flags & CXLINK_NET_FLAG_UPLINK_UP);
		CXLINK_CHECK(prov.sends == 1);

		/*
		 * The bridge rebooting loses them (its copy is RAM-only), reports
		 * "none", and gets them again without the frontend doing anything.
		 */
		{
			CxlinkNetConfig report;

			memset(&report, 0, sizeof(report));
			report.state = CXLINK_NET_STATE_IDLE;
			CXLINK_CHECK(cxlink_test_inject(&link, CXLINK_MSG_NET_CONFIG,
							0, 102,
							(const uint8_t *)&report,
							sizeof(report)));
		}
		cxlink_poll();
		cxlink_test_drain(&link, &log);
		cxlink_net_provision_status(&prov);
		CXLINK_CHECK(!prov.confirmed);
		/* Re-sent immediately, byte for byte. */
		CXLINK_CHECK(log.count == 1);
		CXLINK_CHECK(log.frames[0].type == CXLINK_MSG_NET_CONFIG);
		memcpy(&sent, log.frames[0].payload, sizeof(sent));
		CXLINK_CHECK(!strcmp((const char *)sent.ssid, "BridgeTest"));
		CXLINK_CHECK(!strcmp((const char *)sent.password,
				     "supersecret123"));
		cxlink_net_provision_status(&prov);
		CXLINK_CHECK(prov.pending && prov.sends == 2);

		/*
		 * Dropping them stops both the retransmission and the stored
		 * secret. Checked against the state itself, not just the accessor.
		 */
		cxlink_net_provision_clear();
		for (i = 0; i < sizeof(cxlink.provision_password); i++)
			CXLINK_CHECK(cxlink.provision_password[i] == 0);
		CXLINK_CHECK(cxlink.provision_ssid[0] == 0);
		cxlink_net_provision_status(&prov);
		CXLINK_CHECK(!prov.pending && !prov.confirmed);
		cxlink_poll();
		cxlink_test_drain(&link, &log);
		CXLINK_CHECK(log.count == 0);

		/*
		 * A pair that fails validation must put nothing on the wire at
		 * all: the console reports it instead.
		 */
		CXLINK_CHECK(cxlink_net_provision("", "supersecret123") ==
			     CXLINK_WIFI_PASSWORD_WITHOUT_SSID);
		CXLINK_CHECK(cxlink_net_provision("HomeNet", "short") ==
			     CXLINK_WIFI_PASSWORD_TOO_SHORT);
		cxlink_poll();
		cxlink_test_drain(&link, &log);
		CXLINK_CHECK(log.count == 0);
	}

	/* Leave the global state clean for real use. */
	cxlink_start_null();
	return true;
}

#undef CXLINK_CHECK

#endif /* CXLINK_ENABLE_SELFTEST */
