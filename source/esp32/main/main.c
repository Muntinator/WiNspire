/*
 * cxlink ESP32 firmware - audio and network bridge for WiNspire on the
 * original TI-Nspire CX.
 *
 * This is NOT a co-emulator. The entire PC emulator (CPU, BIOS, VGA, IDE,
 * Sound Blaster, NE2000) runs on the calculator. This firmware is a peripheral:
 *
 *   - audio: Sound Blaster PCM arrives as AUDIO_DATA frames and is pushed to an
 *     I2S DAC/speaker.
 *   - network: the ESP32 is the router the guest's Ethernet cable is plugged
 *     into. It runs a DHCP server for the guest subnet, answers ARP and ICMP for
 *     its own address, forwards the guest's IPv4 traffic onto the Wi-Fi uplink
 *     its station interface is associated with, and source-NATs it there.
 *
 * ARCHITECTURE NOTE ON NETWORKING - read before extending NET_TX handling
 * ---------------------------------------------------------------------
 * A transparent layer-2 bridge is not achievable with stock ESP-IDF.
 * esp_wifi_80211_tx() only injects frames in AP or ESP-NOW modes, and
 * promiscuous RX only sees frames on the current channel, so an ESP32 associated
 * with an existing access point cannot put the guest's MAC address on the wire.
 *
 * What is implemented instead is the closest architecture that keeps the
 * guest-facing NIC unchanged: the ESP32 is a NAT router.
 *
 *   Windows 95 guest  --(192.168.77.0/24 over the UART link)-->  ESP32  --(Wi-Fi
 *   station, address from the access point)-->  LAN / Internet
 *
 * The guest still sees a plain Ethernet adapter, runs its own TCP/IP stack end
 * to end, gets its address by DHCP, resolves its own gateway by ARP, and never
 * learns that the ESP32 exists. The ESP32 does not terminate any guest
 * connection: it forwards IP packets and translates source addresses, which is
 * exactly what a router does. See NETWORK_ARCHITECTURE.md for why this is the
 * ceiling and what it costs.
 *
 * The pieces:
 *   - the guest subnet and its DHCP server live in source/winspire/bridge_net.c,
 *     which is shared source, freestanding, and unit tested by the host build
 *     (`make selftest`). It is the only part of this firmware that can be
 *     executed without hardware, so the DHCP logic deliberately lives there
 *     rather than in an lwIP app.
 *   - the guest-facing interface is a raw lwIP netif built below. lwIP owns its
 *     ARP cache, its ICMP replies and its routing; IP_FORWARD + IPV4_NAPT
 *     (CONFIG_LWIP_IP_FORWARD, CONFIG_LWIP_IPV4_NAPT) do the forwarding and the
 *     source translation.
 *
 * Wire format: the shared header source/winspire/cxlink.h. The ESP-IDF build
 * adds source/winspire to its include path so both sides cannot drift.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "driver/i2s_std.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/opt.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/lwip_napt.h"

#include "cxlink.h"
#include "bridge_net.h"

/*
 * The bridge is a router; without these two symbols lwIP has no forward path and
 * no source translation, and the firmware would compile into something that
 * cannot reach the network at all. Fail loudly instead - the settings are in
 * source/esp32/sdkconfig.defaults.
 */
#if !IP_FORWARD
#error "Enable CONFIG_LWIP_IP_FORWARD (see source/esp32/sdkconfig.defaults)"
#endif
#if !IP_NAPT
#error "Enable CONFIG_LWIP_IPV4_NAPT (see source/esp32/sdkconfig.defaults)"
#endif

static const char *TAG = "cxlink";

/* ------------------------------------------------------------------ */
/* Link configuration                                                  */
/* ------------------------------------------------------------------ */
/*
 * The calculator's dock connector is Tx pin 4 / Rx pin 3 / GND pin 5 at TTL
 * levels (never RS232). Wire calculator Tx -> ESP32 GPIO CXLINK_UART_RX_PIN and
 * calculator Rx -> ESP32 GPIO CXLINK_UART_TX_PIN.
 */
#define CXLINK_UART_PORT UART_NUM_1
#define CXLINK_UART_TX_PIN 17
#define CXLINK_UART_RX_PIN 16
#define CXLINK_UART_BAUD 115200
#define CXLINK_UART_RX_BUF 2048
#define CXLINK_UART_TX_BUF 4096

/* I2S output. Change the pins to match the board. */
#define CXLINK_I2S_BCLK_PIN 26
#define CXLINK_I2S_WS_PIN 25
#define CXLINK_I2S_DOUT_PIN 22

/* Audio block queuing: 32 blocks of up to 128 bytes. */
#define CXLINK_AUDIO_RING_BYTES 8192
#define CXLINK_AUDIO_RING_ITEMS 32

/*
 * The link task does a decoded frame's worth of work per iteration: the decoder
 * holds a whole frame (CXLINK_MAX_FRAME), the dispatch buffer another, the DHCP
 * server a reply, and the outbound encoder a third. That adds up to well over
 * 4 KiB of live stack, which is what the first revision had - so give the task
 * room rather than trimming buffers, because a stack overflow here would look
 * like random corruption in a DHCP reply.
 */
#define CXLINK_TASK_STACK 8192

/* Bumped by hand; reported to the calculator in VERSION frames. */
const uint8_t firmware_build_id[] = "cxlink-esp32-cx-1.1.0";

/*
 * Platform clock required by cxlink.c (declared in pc.h on the calculator
 * side, which this firmware does not include). It drives the reliable channel's
 * retransmit timer and the keepalive, via cxlink_now_ms().
 *
 * esp_timer_get_time() is microseconds since boot as int64. The 32-bit
 * truncation is deliberate and matches the calculator: both ends wrap, and
 * cxlink only ever subtracts two readings, so the wrap is harmless.
 */
uint32_t get_uticks(void)
{
	return (uint32_t)esp_timer_get_time();
}

static uint32_t get_uticks_ms(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ */
/* UART transport                                                      */
/* ------------------------------------------------------------------ */

static size_t uart_hal_write(void *context, const uint8_t *data, size_t length)
{
	int written = uart_write_bytes(CXLINK_UART_PORT, (const char *)data,
				       length);

	(void)context;
	return written > 0 ? (size_t)written : 0;
}

static size_t uart_hal_read(void *context, uint8_t *data, size_t capacity)
{
	int read = uart_read_bytes(CXLINK_UART_PORT, data, capacity, 0);

	(void)context;
	return read > 0 ? (size_t)read : 0;
}

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
	RingbufHandle_t ring;
	i2s_chan_handle_t tx;
	CxlinkAudioConfig config;
	bool configured;
	uint16_t underruns;
	uint32_t blocks;
} AudioPath;

static AudioPath audio;

static esp_err_t audio_configure(const CxlinkAudioConfig *config)
{
	i2s_chan_config_t chan_cfg;
	i2s_std_config_t std_cfg;
	i2s_data_bit_width_t bits;
	i2s_slot_mode_t slots;
	unsigned rate;

	if (audio.configured && config->sample_rate == audio.config.sample_rate &&
	    config->channels == audio.config.channels &&
	    config->format == audio.config.format)
		return ESP_OK;

	if (audio.tx) {
		i2s_channel_disable(audio.tx);
		i2s_del_channel(audio.tx);
		audio.tx = NULL;
	}

	rate = config->sample_rate ? config->sample_rate : 8000;
	bits = config->format == CXLINK_AUDIO_FMT_S16 ?
		       I2S_DATA_BIT_WIDTH_16BIT : I2S_DATA_BIT_WIDTH_8BIT;
	slots = config->channels == 2 ? I2S_SLOT_MODE_STEREO :
					I2S_SLOT_MODE_MONO;

	chan_cfg = (i2s_chan_config_t)I2S_CHANNEL_DEFAULT_CONFIG(0, I2S_ROLE_MASTER);
	if (i2s_new_channel(&chan_cfg, &audio.tx, NULL) != ESP_OK) {
		ESP_LOGE(TAG, "i2s_new_channel failed");
		return ESP_FAIL;
	}
	std_cfg = (i2s_std_config_t){
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, slots),
		.gpio_cfg = {
			.mclk = I2S_GPIO_UNUSED,
			.bclk = CXLINK_I2S_BCLK_PIN,
			.ws = CXLINK_I2S_WS_PIN,
			.dout = CXLINK_I2S_DOUT_PIN,
			.din = I2S_GPIO_UNUSED,
			.invert_flags = {
				.mclk_inv = false,
				.bclk_inv = false,
				.ws_inv = false,
			},
		},
	};
	if (i2s_channel_init_std_mode(audio.tx, &std_cfg) != ESP_OK) {
		ESP_LOGE(TAG, "i2s_channel_init_std_mode failed");
		return ESP_FAIL;
	}
	i2s_channel_enable(audio.tx);

	audio.config = *config;
	audio.configured = true;
	audio.underruns = 0;
	ESP_LOGI(TAG, "audio: %u Hz, %u ch, %u-bit", (unsigned)rate,
		 (unsigned)config->channels, config->format == CXLINK_AUDIO_FMT_S16 ? 16u : 8u);
	return ESP_OK;
}

static void audio_pump(void)
{
	size_t length = 0;
	void *item;
	size_t written = 0;

	if (!audio.configured)
		return;
	/*
	 * Non-blocking drain: if the DAC cannot take the block right now, count an
	 * underrun rather than blocking the link task (which would back-pressure
	 * the UART and stall the calculator).
	 */
	item = xRingbufferReceive(audio.ring, &length, 0);
	if (!item)
		return;
	if (i2s_channel_write(audio.tx, item, length, &written, 0) != ESP_OK ||
	    written != length)
		audio.underruns++;
	audio.blocks++;
	vRingbufferReturnItem(audio.ring, item);
}

/* ------------------------------------------------------------------ */
/* Guest-side frames                                                   */
/* ------------------------------------------------------------------ */

/*
 * Send an Ethernet frame to the calculator as one NET_RX frame. The single exit
 * for everything that goes towards the guest: DHCP replies written by
 * bridge_net.c, and frames lwIP hands to the netif's linkoutput.
 *
 * It is called from two tasks - the link task (DHCP) and the TCP/IP task (lwIP's
 * output path) - so it keeps its encoder buffer on the stack. uart_write_bytes()
 * is safe from both.
 */
static void cxlink_send_net_rx(const uint8_t *frame, size_t length)
{
	static uint16_t net_rx_sequence;
	uint8_t out[CXLINK_MAX_FRAME];
	size_t total;

	if (!length || length > CXLINK_MAX_PAYLOAD)
		return;
	total = cxlink_encode(CXLINK_MSG_NET_RX, 0, net_rx_sequence++, frame,
			      length, out, sizeof(out));
	if (total)
		(void)uart_hal_write(NULL, out, total);
}

static void net_emit_frame(void *context, const uint8_t *frame, size_t length)
{
	(void)context;
	cxlink_send_net_rx(frame, length);
}

/* ------------------------------------------------------------------ */
/* The router                                                          */
/* ------------------------------------------------------------------ */
/*
 * The guest-facing lwIP netif. Everything else is lwIP's: it answers ARP for
 * 192.168.77.1, answers ICMP echo, keeps the ARP cache that the return path
 * needs, routes the guest's off-subnet traffic to the station interface, and
 * source-NATs it there (ip_napt_forward, armed by net_set_napt()).
 *
 * lwIP's own forwarding hook applies NAPT when the *input* netif has napt set,
 * so NAPT is enabled on this netif and deliberately not on the station one: for
 * an outbound packet this netif is the input, and for a reply arriving on the
 * station interface ip4_input calls ip_napt_recv() to translate the destination
 * back to the guest's address. Enabling it on both would stop the two halves
 * matching up, which is why net_set_napt() only ever touches this one.
 */
static struct netif cx_netif;
static bool cx_netif_ready;
static volatile bool net_uplink_up;
static volatile bool net_dns_dirty;
static bool net_napt_enabled;
static esp_netif_t *net_uplink;
/*
 * Uplink provisioning state, all of it RAM only. net_wifi_configured means a
 * validated credential pair was handed to the Wi-Fi driver, which is what the
 * calculator waits for before it stops re-sending them; net_wifi_state is that
 * plus what the radio has done with them, and is reported in every NET_CONFIG.
 */
static volatile bool net_wifi_configured;
static volatile uint8_t net_wifi_state = CXLINK_NET_STATE_IDLE;

/* Counters, reported to the calculator through CXLINK_MSG_NET_STATUS. */
static uint32_t net_rx_frames; /* uplink -> guest */
static uint32_t net_tx_frames; /* guest -> uplink */
static uint32_t net_dropped;
static uint32_t net_duplicate_frames;

/* The last NET_TX sequence accepted, so a retransmission can be recognised. */
static uint16_t net_retry_sequence;
static bool net_retry_seen;

static BridgeNetDhcp dhcp;

static err_t cx_netif_linkoutput(struct netif *netif, struct pbuf *p)
{
	uint8_t frame[CXLINK_MAX_PAYLOAD];
	struct pbuf *part;
	size_t offset = 0;

	if (!p || netif->hwaddr_len != NETIF_MAX_HWADDR_LEN)
		return ERR_ARG;

	/*
	 * For an Ethernet interface lwIP passes linkoutput a pbuf that already
	 * begins with the Ethernet header, so this is a copy, not a header
	 * prepend.
	 */
	for (part = p; part != NULL; part = part->next) {
		if (part->len > sizeof(frame) - offset)
			return ERR_MEM;
		memcpy(frame + offset, part->payload, part->len);
		offset += part->len;
	}
	if (!offset || offset > sizeof(frame))
		return ERR_MEM;
	cxlink_send_net_rx(frame, offset);
	net_rx_frames++;
	return ERR_OK;
}

static err_t cx_netif_init(struct netif *netif)
{
	netif->name[0] = 'c';
	netif->name[1] = 'x';
	netif->output = etharp_output;
	netif->linkoutput = cx_netif_linkoutput;
	/*
	 * cxlink carries at most CXLINK_MAX_PAYLOAD bytes per frame and an
	 * Ethernet frame includes its 14-byte header, so this MTU is the largest
	 * datagram whose frame still fits. Anything larger arriving from the
	 * uplink is fragmented by lwIP on the way here rather than dropped.
	 */
	netif->mtu = CXLINK_MAX_PAYLOAD - BRIDGE_NET_ETH_HEADER_LEN;
	netif->hwaddr_len = NETIF_MAX_HWADDR_LEN;
	memcpy(netif->hwaddr, dhcp.config.mac, NETIF_MAX_HWADDR_LEN);
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
		       NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
	return ERR_OK;
}

/*
 * Hand one guest frame to lwIP. Runs on the link task, so the pbuf is built here
 * and posted to the TCP/IP task by tcpip_input (this build has
 * LWIP_TCPIP_CORE_LOCKING_INPUT off, so netif->input queues the packet rather
 * than touching core state from the caller's context).
 */
static bool net_stack_input(const uint8_t *frame, size_t length)
{
	struct pbuf *packet;
	err_t error;

	if (!cx_netif_ready || length < BRIDGE_NET_ETH_HEADER_LEN ||
	    length > CXLINK_MAX_PAYLOAD)
		return false;

	packet = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_RAM);
	if (!packet)
		return false;
	if (pbuf_take(packet, frame, (u16_t)length) != ERR_OK) {
		pbuf_free(packet);
		return false;
	}
	error = cx_netif.input(packet, &cx_netif);
	if (error != ERR_OK) {
		/* Not queued, so lwIP never took ownership of the pbuf. */
		pbuf_free(packet);
		return false;
	}
	return true;
}

/*
 * The routing backend: one IPv4 datagram from the guest, on its way out.
 *
 * lwIP receives the whole Ethernet frame rather than the bare datagram because
 * ethernet_input() is what validates the header and learns the guest's hardware
 * address, which the return path needs before the NAT table can hand anything
 * back. From there it is lwIP's routing: on-subnet destinations are answered
 * locally, everything else leaves through the station interface with the source
 * rewritten by ip_napt_forward().
 *
 * A false return means the packet was dropped, not that the caller should try
 * something else: there is nowhere else for it to go.
 */
static bool net_forward_ipv4(const uint8_t *frame, size_t length)
{
	if (length < BRIDGE_NET_ETH_HEADER_LEN + 20u ||
	    (frame[BRIDGE_NET_ETH_HEADER_LEN] >> 4) != 4u)
		return false;
	return net_stack_input(frame, length);
}

static void net_handle_tx(const uint8_t *frame, size_t length)
{
	BridgeNetAction action = bridge_net_handle_ethernet(
		&dhcp, get_uticks_ms(), frame, length, net_emit_frame, NULL);
	bool accepted;

	if (action == BRIDGE_NET_HANDLED || action == BRIDGE_NET_DROP)
		return;
	if (action == BRIDGE_NET_IPV4)
		accepted = net_forward_ipv4(frame, length);
	else
		/* ARP and anything else link-layer: lwIP owns the link layer of
		 * this interface and has to see it. */
		accepted = net_stack_input(frame, length);
	if (accepted)
		net_tx_frames++;
	else
		net_dropped++;
}

/* ------------------------------------------------------------------ */
/* Uplink: Wi-Fi station, NAPT, DNS                                    */
/* ------------------------------------------------------------------ */

/* Defined with the credential handling; used by the event handler below. */
static uint8_t net_state_for_reason(uint8_t reason);

static void net_on_wifi_event(void *context, esp_event_base_t base, int32_t id,
			      void *data)
{
	(void)context;

	if (base != WIFI_EVENT)
		return;
	switch (id) {
	case WIFI_EVENT_STA_START:
		/* Nothing to associate with until the calculator provisions us;
		 * connecting with an empty SSID would just spin. */
		if (net_wifi_configured)
			(void)esp_wifi_connect();
		break;
	case WIFI_EVENT_STA_DISCONNECTED:
	{
		const wifi_event_sta_disconnected_t *disconnected = data;

		net_uplink_up = false;
		if (disconnected) {
			const uint8_t state = net_state_for_reason(
				disconnected->reason);

			if (state == CXLINK_NET_STATE_REJECTED &&
			    net_wifi_state != CXLINK_NET_STATE_REJECTED)
				ESP_LOGW(TAG, "uplink refused (reason %u); "
					      "check the ssid and password",
					 (unsigned)disconnected->reason);
			net_wifi_state = state;
		}
		/*
		 * Retry forever. The only reason the uplink exists is so that the
		 * guest can reach the network, and the guest cannot tell us when
		 * it wants to; a bridge that gives up after N attempts would need
		 * a power cycle after the access point restarts.
		 */
		if (net_wifi_configured) {
			ESP_LOGW(TAG, "wifi: disconnected, reconnecting");
			(void)esp_wifi_connect();
		}
		break;
	}
	default:
		break;
	}
}

/*
 * Called from the link task, never from an event handler: it makes blocking IPC
 * calls into the TCP/IP task, and the event task must not be able to wait on the
 * task that feeds it events.
 */
static void net_refresh_dns(void)
{
	esp_netif_dns_info_t info;

	if (!net_uplink)
		return;
	if (esp_netif_get_dns_info(net_uplink, ESP_NETIF_DNS_MAIN, &info) !=
	    ESP_OK)
		return;
	/* esp_ip4_addr_t.addr is already in network byte order, which is exactly
	 * the order the DHCP option needs, so this is a copy and not a conversion. */
	if (!info.ip.u_addr.ip4.addr)
		return;
	memcpy(dhcp.config.dns, &info.ip.u_addr.ip4.addr, BRIDGE_NET_ADDR_LEN);
	ESP_LOGI(TAG, "uplink dns %u.%u.%u.%u",
		 (unsigned)dhcp.config.dns[0], (unsigned)dhcp.config.dns[1],
		 (unsigned)dhcp.config.dns[2], (unsigned)dhcp.config.dns[3]);
}

static void net_on_ip_event(void *context, esp_event_base_t base, int32_t id,
			    void *data)
{
	(void)context;

	if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP)
		return;
	(void)data;
	/*
	 * Flags only. Everything that follows from the uplink coming up - enabling
	 * NAPT, reading the upstream resolver - makes blocking IPC calls into the
	 * TCP/IP task, which is the task that posts these events; doing that here
	 * would be a deadlock waiting for a full event queue. net_service() runs
	 * it from the link task instead.
	 */
	net_uplink_up = true;
	net_dns_dirty = true;
	net_wifi_state = CXLINK_NET_STATE_CONNECTED;
}

static esp_err_t net_napt_exec(void *context)
{
	bool enable = *(const bool *)context;

	if (cx_netif_ready &&
	    !ip_napt_enable_netif(&cx_netif, enable ? 1 : 0))
		return ESP_FAIL;
	ESP_LOGI(TAG, "napt %s on the guest interface", enable ? "on" : "off");
	return ESP_OK;
}

/*
 * Enabling NAPT allocates tables and touches the netif list, so it has to happen
 * on the TCP/IP task. esp_netif_tcpip_exec() blocks until the callback has run,
 * which is what makes passing a pointer to a local safe.
 */
static bool net_set_napt(bool enable)
{
	return esp_netif_tcpip_exec(net_napt_exec, &enable) == ESP_OK;
}

/* Reconcile the router with the uplink. Runs on the link task. */
static void net_service(void)
{
	bool uplink = net_uplink_up;

	if (uplink != net_napt_enabled && net_set_napt(uplink))
		net_napt_enabled = uplink;
	if (net_dns_dirty) {
		net_dns_dirty = false;
		net_refresh_dns();
	}
}

static esp_err_t net_router_exec(void *context)
{
	ip4_addr_t address;
	ip4_addr_t netmask;
	ip4_addr_t gateway;

	(void)context;
	IP4_ADDR(&address, dhcp.config.address[0], dhcp.config.address[1],
		 dhcp.config.address[2], dhcp.config.address[3]);
	IP4_ADDR(&netmask, dhcp.config.netmask[0], dhcp.config.netmask[1],
		 dhcp.config.netmask[2], dhcp.config.netmask[3]);
	gateway = address;

	/* netif_add() fills in only part of the structure, so start from zero. */
	memset(&cx_netif, 0, sizeof(cx_netif));
	if (!netif_add(&cx_netif, &address, &netmask, &gateway, NULL,
		       cx_netif_init, tcpip_input)) {
		ESP_LOGE(TAG, "guest interface could not be added");
		return ESP_FAIL;
	}
	netif_set_up(&cx_netif);
	netif_set_link_up(&cx_netif);
	/*
	 * Deliberately NOT the default interface: everything that is not on the
	 * guest subnet has to leave through the uplink, and a default netif with
	 * no address would swallow the guest's traffic before the station has
	 * associated.
	 */
	cx_netif_ready = true;
	ESP_LOGI(TAG, "guest lan %u.%u.%u.%u/%u.%u.%u.%u up",
		 (unsigned)dhcp.config.address[0],
		 (unsigned)dhcp.config.address[1],
		 (unsigned)dhcp.config.address[2],
		 (unsigned)dhcp.config.address[3],
		 (unsigned)dhcp.config.netmask[0],
		 (unsigned)dhcp.config.netmask[1],
		 (unsigned)dhcp.config.netmask[2],
		 (unsigned)dhcp.config.netmask[3]);
	return ESP_OK;
}

/*
 * Copy the NUL-terminated string inside a fixed field, never past what the
 * destination can hold. Both of these come off the link, so neither length is
 * trusted, and the destination is the Wi-Fi driver's own fixed-size field rather
 * than cxlink's slightly larger one.
 */
static size_t net_copy_string(uint8_t *destination, size_t capacity,
			      const uint8_t *source, size_t source_size)
{
	size_t length = strnlen((const char *)source, source_size);

	if (length > capacity)
		length = capacity;
	memcpy(destination, source, length);
	return length;
}

/*
 * Apply the calculator's uplink settings. Called with the NET_CONFIG frame's
 * payload, which is where the access point's SSID and password arrive from: the
 * calculator is the bridge's only console, and a password in source-controlled
 * build configuration would be a password in git.
 *
 * The password is never logged, echoed or stored outside this function's local
 * config: the only trace of a successful apply is the SSID.
 */
static void net_apply_uplink(const CxlinkNetConfig *config)
{
	/* The pair actually in use, so a retransmission is a no-op. Both are
	 * compared: a password change that keeps the SSID is a change. */
	static uint8_t current_ssid[CXLINK_WIFI_SSID_MAX];
	static uint8_t current_password[CXLINK_WIFI_PASSWORD_MAX];
	wifi_config_t wifi_config;
	CxlinkWifiResult result;
	size_t ssid_length;

	if (!config->ssid[0]) {
		/* Empty means "no change", so a frontend that only reports
		 * uplink state does not wipe the credentials. */
		return;
	}
	/*
	 * Validate before touching the Wi-Fi driver. The calculator runs the same
	 * check, so a refusal here means an older frontend or a corrupted frame -
	 * either way the credentials are not applied, and saying so is better than
	 * an association failure the user cannot interpret.
	 */
	result = cxlink_wifi_check((const char *)config->ssid,
				   (const char *)config->password);
	if (result != CXLINK_WIFI_OK) {
		ESP_LOGE(TAG, "uplink credentials refused: %s",
			 cxlink_wifi_result_text(result));
		return;
	}
	if (net_wifi_configured &&
	    memcmp(current_ssid, config->ssid, sizeof(current_ssid)) == 0 &&
	    memcmp(current_password, config->password,
		   sizeof(current_password)) == 0) {
		/* Already using this pair: a retransmission, not a change. */
		return;
	}

	memset(&wifi_config, 0, sizeof(wifi_config));
	ssid_length = net_copy_string(wifi_config.sta.ssid,
				      sizeof(wifi_config.sta.ssid), config->ssid,
				      sizeof(config->ssid));
	(void)net_copy_string(wifi_config.sta.password,
			      sizeof(wifi_config.sta.password), config->password,
			      sizeof(config->password));
	/* Accept whatever the access point offers; the threshold exists for
	 * deployments that forbid weak encryption, which this is not. */
	wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
	wifi_config.sta.pmf_cfg.capable = true;
	wifi_config.sta.pmf_cfg.required = false;

	if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
		ESP_LOGE(TAG, "could not set the uplink credentials");
		return;
	}
	memcpy(current_ssid, config->ssid, sizeof(current_ssid));
	memcpy(current_password, config->password, sizeof(current_password));
	net_wifi_configured = true;
	net_wifi_state = CXLINK_NET_STATE_ASSOCIATING;
	ESP_LOGI(TAG, "uplink ssid \"%.*s\"", (int)ssid_length,
		 (const char *)config->ssid);
	/* Re-associating is harmless when already connected to this SSID, and it
	 * is what makes a credential change take effect without a reset. */
	(void)esp_wifi_disconnect();
	(void)esp_wifi_connect();
}

/*
 * Which failures mean "these credentials are wrong" rather than "the access
 * point is not reachable yet". The distinction is the difference between a user
 * retyping a password and a user walking closer to the router, so it is worth
 * the enum fiddling.
 */
static uint8_t net_state_for_reason(uint8_t reason)
{
	switch (reason) {
	case WIFI_REASON_AUTH_EXPIRE:
	case WIFI_REASON_MIC_FAILURE:
	case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
	case WIFI_REASON_AUTH_FAIL:
	case WIFI_REASON_ASSOC_FAIL:
	case WIFI_REASON_HANDSHAKE_TIMEOUT:
		return CXLINK_NET_STATE_REJECTED;
	default:
		break;
	}
	return CXLINK_NET_STATE_ASSOCIATING;
}

/*
 * The bridge's half of the NET_CONFIG exchange: its MAC, whether it holds
 * credentials, whether the uplink is up, and what the radio has done with them.
 * Sent every 2 s, which is what makes the calculator's "stop resending once it
 * is confirmed" logic work and what re-provisions a bridge that was restarted.
 */
static void netsend_config(void)
{
	CxlinkNetConfig config;
	uint8_t out[CXLINK_MAX_FRAME];
	static uint16_t sequence;
	size_t total;

	memset(&config, 0, sizeof(config));
	memcpy(config.mac, dhcp.config.mac, sizeof(config.mac));
	if (net_uplink_up)
		config.flags |= CXLINK_NET_FLAG_UPLINK_UP;
	if (net_wifi_configured)
		config.flags |= CXLINK_NET_FLAG_CREDENTIALS;
	config.state = net_wifi_state;
	total = cxlink_encode(CXLINK_MSG_NET_CONFIG, 0, sequence++,
			      (uint8_t *)&config, sizeof(config), out,
			      sizeof(out));
	if (total)
		(void)uart_hal_write(NULL, out, total);
}

static void netsend_status(void)
{
	CxlinkNetStatus status;
	uint8_t out[CXLINK_MAX_FRAME];
	static uint16_t sequence;
	size_t total;

	memset(&status, 0, sizeof(status));
	status.tx_frames = net_tx_frames;
	status.rx_frames = net_rx_frames;
	status.tx_dropped = (uint16_t)(net_dropped > 0xFFFFu ?
					       0xFFFFu : net_dropped);
	status.link_errors = (uint16_t)(net_duplicate_frames > 0xFFFFu ?
						0xFFFFu : net_duplicate_frames);
	total = cxlink_encode(CXLINK_MSG_NET_STATUS, 0, sequence++,
			      (uint8_t *)&status, sizeof(status), out,
			      sizeof(out));
	if (total)
		(void)uart_hal_write(NULL, out, total);
}

static void wifi_setup(void)
{
	wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(esp_wifi_init(&init));
	/* Nothing here is worth a flash write: the credentials come from the
	 * calculator over the link and are re-sent on every boot. */
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
						   ESP_EVENT_ANY_ID,
						   net_on_wifi_event, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
						   IP_EVENT_STA_GOT_IP,
						   net_on_ip_event, NULL));

	net_uplink = esp_netif_create_default_wifi_sta();
	if (!net_uplink)
		ESP_ERROR_CHECK(ESP_FAIL);
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
}

/* ------------------------------------------------------------------ */
/* Frame dispatch                                                      */
/* ------------------------------------------------------------------ */

static uint16_t mgmt_sequence;

static void send_audio_status(void)
{
	CxlinkAudioStatus status;
	uint8_t out[CXLINK_MAX_FRAME];
	size_t total;

	/*
	 * queued_frames is reported as the free space left in the audio ring in
	 * blocks, so the calculator can see the buffer draining before it turns
	 * into an underrun.
	 */
	memset(&status, 0, sizeof(status));
	status.queued_frames = audio.ring ?
		(uint16_t)(xRingbufferGetCurFreeSize(audio.ring) /
			   (CXLINK_AUDIO_RING_BYTES / CXLINK_AUDIO_RING_ITEMS)) : 0;
	status.underruns = audio.underruns;
	total = cxlink_encode(CXLINK_MSG_AUDIO_STATUS, 0, mgmt_sequence++,
			      (uint8_t *)&status, sizeof(status), out, sizeof(out));
	if (total)
		(void)uart_hal_write(NULL, out, total);
}

static void send_version(void)
{
	CxlinkVersion version = {
		.major = CXLINK_VERSION_MAJOR,
		.minor = CXLINK_VERSION_MINOR,
		.firmware = { 'E', 'S', 'P', '3' },
	};
	uint8_t out[CXLINK_MAX_FRAME];
	size_t total = cxlink_encode(CXLINK_MSG_VERSION, 0, mgmt_sequence++,
				     (uint8_t *)&version, sizeof(version), out,
				     sizeof(out));

	if (total)
		(void)uart_hal_write(NULL, out, total);
}

static void send_error(uint8_t code)
{
	uint8_t out[CXLINK_MAX_FRAME];
	size_t total = cxlink_encode(CXLINK_MSG_ERROR, CXLINK_FLAG_ERROR, 0,
				     &code, 1, out, sizeof(out));

	if (total)
		(void)uart_hal_write(NULL, out, total);
}

static void handle_frame(const CxlinkFrame *frame)
{
	uint8_t out[CXLINK_MAX_FRAME];

	switch (frame->type) {
	case CXLINK_MSG_HELLO:
		send_version();
		netsend_config();
		break;

	case CXLINK_MSG_PING:
		/* Echo the payload and the sequence number back as PONG. */
		{
			size_t total = cxlink_encode(CXLINK_MSG_PONG, 0,
						     frame->sequence, frame->payload,
						     frame->length, out, sizeof(out));

			if (total)
				(void)uart_hal_write(NULL, out, total);
		}
		break;

	case CXLINK_MSG_AUDIO_CONFIG:
		if (frame->length >= sizeof(CxlinkAudioConfig) &&
		    audio_configure((const CxlinkAudioConfig *)frame->payload) !=
			    ESP_OK)
			send_error(1); /* bad audio configuration */
		send_audio_status();
		break;

	case CXLINK_MSG_AUDIO_DATA:
		if (audio.configured && frame->length &&
		    xRingbufferSend(audio.ring, frame->payload, frame->length,
				    0) != pdTRUE)
			audio.underruns++;
		break;

	case CXLINK_MSG_AUDIO_FLUSH:
		/* Drop whatever is buffered so the guest can restart cleanly. */
		{
			size_t length;
			void *item;

			while ((item = xRingbufferReceive(audio.ring, &length, 0)))
				vRingbufferReturnItem(audio.ring, item);
		}
		send_audio_status();
		break;

	case CXLINK_MSG_NET_TX:
		/*
		 * The reliable channel is a window of one, so the only frame that can
		 * legitimately arrive twice is the one still outstanding. A
		 * retransmission still needs its ACK - that is the whole point - but
		 * forwarding it would put a duplicate frame on the network and, for
		 * DHCP, elicit a duplicate reply.
		 *
		 * An unrecognised sequence on a RETRY is forwarded: it can only mean
		 * this firmware restarted and lost its place, and dropping a real
		 * frame would be worse than a duplicate.
		 */
		if ((frame->flags & CXLINK_FLAG_RETRY) && net_retry_seen &&
		    net_retry_sequence == frame->sequence)
			net_duplicate_frames++;
		else {
			net_handle_tx(frame->payload, frame->length);
			net_retry_sequence = frame->sequence;
			net_retry_seen = true;
		}
		if (frame->flags & CXLINK_FLAG_ACK_REQ) {
			uint8_t ack[4];

			ack[0] = frame->type;
			ack[1] = frame->flags;
			/*
			 * cxlink's own wire format is little-endian, unlike the IP
			 * headers bridge_net.c builds, and this encoding is what
			 * cxlink.c decodes (payload[2] | payload[3] << 8). Writing
			 * it big-endian - which this firmware used to do - means the
			 * calculator never matches the ACK to the frame it is
			 * waiting for, retransmits six times, and declares the link
			 * down.
			 */
			ack[2] = (uint8_t)frame->sequence;
			ack[3] = (uint8_t)(frame->sequence >> 8);
			if (cxlink_encode(CXLINK_MSG_ACK, 0, mgmt_sequence++,
					  ack, sizeof(ack), out, sizeof(out)))
				(void)uart_hal_write(NULL, out,
						     CXLINK_FRAME_OVERHEAD +
							     sizeof(ack));
		}
		break;

	case CXLINK_MSG_NET_CONFIG:
		if (frame->length >= sizeof(CxlinkNetConfig))
			net_apply_uplink((const CxlinkNetConfig *)frame->payload);
		netsend_config();
		break;

	case CXLINK_MSG_NET_STATUS:
		netsend_status();
		break;

	case CXLINK_MSG_STATUS:
		break;

	case CXLINK_MSG_RESET:
		esp_restart();
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/* ------------------------------------------------------------------ */

static void cxlink_task(void *arg)
{
	CxlinkDecoder decoder;
	uint8_t chunk[128];
	uint32_t next_report_ms;

	(void)arg;
	cxlink_decoder_init(&decoder);
	/*
	 * Announce ourselves immediately so the calculator does not have to poll
	 * for the bridge, and so a hot-plugged ESP32 resolves without a reset.
	 */
	send_version();
	netsend_config();
	next_report_ms = get_uticks_ms() + 2000u;

	for (;;) {
		size_t got = uart_hal_read(NULL, chunk, sizeof(chunk));
		size_t i;
		uint32_t now;

		for (i = 0; i < got; i++) {
			const CxlinkFrame *frame = NULL;

			if (cxlink_decoder_push(&decoder, chunk[i], &frame) &&
			    frame)
				handle_frame(frame);
		}
		audio_pump();

		net_service();

		/* Tell the calculator whether the uplink is up: it has no other
		 * way to know, and NET_CONFIG is what feeds cxlink_link_up(). */
		now = get_uticks_ms();
		if ((int32_t)(now - next_report_ms) >= 0) {
			next_report_ms = now + 2000u;
			netsend_config();
			/* The upstream resolver can appear a moment after the
			 * address does, so keep refreshing while the uplink is up. */
			if (net_uplink_up)
				net_dns_dirty = true;
		}
		/* Yield briefly; the UART RX ring buffers while we sleep. */
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

static void uart_setup(void)
{
	uart_config_t config = {
		.baud_rate = CXLINK_UART_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	ESP_ERROR_CHECK(uart_driver_install(CXLINK_UART_PORT, CXLINK_UART_RX_BUF,
					    CXLINK_UART_TX_BUF, 0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(CXLINK_UART_PORT, &config));
	ESP_ERROR_CHECK(uart_set_pin(CXLINK_UART_PORT, CXLINK_UART_TX_PIN,
				     CXLINK_UART_RX_PIN, UART_PIN_NO_CHANGE,
				     UART_PIN_NO_CHANGE));
}

void app_main(void)
{
	esp_err_t nvs = nvs_flash_init();

	if (nvs == ESP_ERR_NVS_NO_FREE_PAGES ||
	    nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ESP_ERROR_CHECK(nvs_flash_init());
	}
	ESP_LOGI(TAG, "cxlink bridge, build %s", (const char *)firmware_build_id);

	uart_setup();
	audio.ring = xRingbufferCreate(CXLINK_AUDIO_RING_BYTES,
				       RINGBUF_TYPE_NOSPLIT);
	if (!audio.ring)
		ESP_LOGE(TAG, "audio ring allocation failed");

	/* The guest subnet and its DHCP server, with the built-in defaults. */
	bridge_net_dhcp_init(&dhcp, NULL);

	/*
	 * Bring up Wi-Fi first: it creates the TCP/IP task and the station
	 * interface the guest's traffic leaves through. The guest interface is
	 * then added from the TCP/IP task, because that is the only context that
	 * may touch the netif list.
	 */
	wifi_setup();
	ESP_ERROR_CHECK(esp_netif_tcpip_exec(net_router_exec, NULL));

	xTaskCreate(cxlink_task, "cxlink", CXLINK_TASK_STACK, NULL, 10, NULL);
}
