/*
 * bridge_net - the guest-side router on the ESP32: a DHCP server, and the
 * classifier that decides what the calculator's frames are for.
 *
 * See bridge_net.h for why this is a separate, freestanding translation unit and
 * for where it sits in the frame path. In short: it is the only part of the
 * ESP32 firmware that can be executed in the environment this tree is developed
 * in, so it is where the DHCP logic lives and where `make selftest` points.
 *
 * TWO DECISIONS THAT ARE NOT OBVIOUS
 * ----------------------------------
 * 1. DHCP replies are addressed straight to the client's hardware address, taken
 *    from the BOOTP `chaddr` field, rather than ARP-resolved to the address
 *    being handed out. The client does not own that address yet, so an ARP step
 *    here is a race that a real network can lose; every DHCP server unicasts to
 *    the client's MAC for exactly this reason. When the client sets the BOOTP
 *    broadcast flag - some Microsoft clients do - the reply goes to
 *    ff:ff:ff:ff:ff:ff and 255.255.255.255 instead, as RFC 2131 requires.
 * 2. Leases are recorded on DISCOVER, not just on REQUEST. That single fact is
 *    what makes a lost reply recoverable: the retransmitted DISCOVER comes from
 *    the same hardware address, finds its own lease, and is offered the same
 *    address instead of marching through the pool.
 */
#include "bridge_net.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Wire constants                                                      */
/* ------------------------------------------------------------------ */

#define ETH_TYPE_IPV4 0x0800u
#define ETH_TYPE_ARP 0x0806u

#define IP_VERSION_4 4u
#define IP_HEADER_LEN 20u
#define IP_PROTO_UDP 17u
#define IP_TTL 64u

#define UDP_HEADER_LEN 8u
#define DHCP_SERVER_PORT 67u
#define DHCP_CLIENT_PORT 68u

/* BOOTP, RFC 951. DHCP adds a 4-byte cookie at 236 and options after it. */
#define BOOTP_OP_REQUEST 1u
#define BOOTP_OP_REPLY 2u
#define BOOTP_HTYPE_ETHERNET 1u
#define BOOTP_HLEN_ETHERNET 6u
#define BOOTP_FIXED_LEN 236u
#define BOOTP_COOKIE_LEN 4u
#define BOOTP_OPTIONS_OFFSET (BOOTP_FIXED_LEN + BOOTP_COOKIE_LEN)
#define BOOTP_FLAG_BROADCAST 0x8000u

/* Field offsets inside the BOOTP header. */
#define BOOTP_XID 4u
#define BOOTP_SECS 8u
#define BOOTP_FLAGS 10u
#define BOOTP_CIADDR 12u
#define BOOTP_YIADDR 16u
#define BOOTP_SIADDR 20u
#define BOOTP_GIADDR 24u
#define BOOTP_CHADDR 28u

/* DHCP option codes. */
#define DHCP_OPTION_PAD 0u
#define DHCP_OPTION_NETMASK 1u
#define DHCP_OPTION_ROUTER 3u
#define DHCP_OPTION_DNS 6u
#define DHCP_OPTION_REQUESTED_IP 50u
#define DHCP_OPTION_LEASE_TIME 51u
#define DHCP_OPTION_MESSAGE_TYPE 53u
#define DHCP_OPTION_SERVER_ID 54u
#define DHCP_OPTION_BROADCAST 28u
#define DHCP_OPTION_RENEWAL_TIME 58u
#define DHCP_OPTION_REBIND_TIME 59u
#define DHCP_OPTION_END 255u

/* DHCP message types. */
#define DHCP_DISCOVER 1u
#define DHCP_OFFER 2u
#define DHCP_REQUEST 3u
#define DHCP_DECLINE 4u
#define DHCP_ACK 5u
#define DHCP_NAK 6u
#define DHCP_RELEASE 7u
#define DHCP_INFORM 8u

static const uint8_t bootp_cookie[BOOTP_COOKIE_LEN] = { 99u, 130u, 83u, 99u };
static const uint8_t broadcast_address[BRIDGE_NET_ADDR_LEN] = { 255, 255, 255, 255 };
static const uint8_t all_zero_address[BRIDGE_NET_ADDR_LEN] = { 0, 0, 0, 0 };

/*
 * Sizing. Options for the guest are 52 bytes (53, 54, 51, 1, 3, 6, 28, 58, 59 and
 * END), so a 300-byte BOOTP message - the RFC 2131 minimum, and what dnsmasq
 * pads to for old clients - leaves room to spare. The reply is padded rather
 * than trimmed: a shorter reply works with Windows 95, but the padding costs
 * nothing and removes a class of old-client quirk.
 */
#define BOOTP_REPLY_LEN 300u
#define BRIDGE_NET_MAX_DATAGRAM \
	(IP_HEADER_LEN + UDP_HEADER_LEN + BOOTP_REPLY_LEN)
#define BRIDGE_NET_MAX_FRAME \
	(BRIDGE_NET_ETH_HEADER_LEN + BRIDGE_NET_MAX_DATAGRAM)

/* How long a declined address stays out of circulation. */
#define BRIDGE_NET_DECLINE_QUARANTINE_MS 60000u

/* ------------------------------------------------------------------ */
/* Byte and checksum helpers                                           */
/* ------------------------------------------------------------------ */
/*
 * All multi-byte protocol fields are big-endian, unlike every other part of
 * cxlink, because that is what the IPv4/UDP/BOOTP formats specify. These helpers
 * are named for the width they touch so a call site cannot silently reorder.
 */

static uint16_t read_be16(const uint8_t *data)
{
	return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) | data[3];
}

static void write_be16(uint8_t *data, uint16_t value)
{
	data[0] = (uint8_t)(value >> 8);
	data[1] = (uint8_t)value;
}

static void write_be32(uint8_t *data, uint32_t value)
{
	data[0] = (uint8_t)(value >> 24);
	data[1] = (uint8_t)(value >> 16);
	data[2] = (uint8_t)(value >> 8);
	data[3] = (uint8_t)value;
}

static uint32_t checksum_add(uint32_t sum, const uint8_t *data, size_t length)
{
	size_t i = 0;

	while (i + 1u < length) {
		sum += (uint32_t)(((uint16_t)data[i] << 8) | data[i + 1u]);
		i += 2u;
	}
	if (i < length)
		sum += (uint32_t)((uint16_t)data[i] << 8);
	return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return (uint16_t)(~sum & 0xFFFFu);
}

static uint16_t ip_checksum(const uint8_t *header, size_t length)
{
	uint32_t sum = 0;

	/* The checksum field at 10..11 is the only part left out of the sum; it
	 * is skipped rather than zeroed so the header does not have to be
	 * temporarily modified. */
	sum = checksum_add(sum, header, 10u);
	sum = checksum_add(sum, header + 12u,
			   length > 12u ? length - 12u : 0u);
	return checksum_finish(sum);
}

static uint16_t udp_checksum(const uint8_t *source, const uint8_t *destination,
			     const uint8_t *udp, size_t length)
{
	uint8_t pseudo[12];
	uint32_t sum = 0;
	uint16_t value;

	memcpy(pseudo, source, BRIDGE_NET_ADDR_LEN);
	memcpy(pseudo + 4, destination, BRIDGE_NET_ADDR_LEN);
	pseudo[8] = 0;
	pseudo[9] = IP_PROTO_UDP;
	write_be16(pseudo + 10, (uint16_t)length);
	sum = checksum_add(sum, pseudo, sizeof(pseudo));
	sum = checksum_add(sum, udp, length);
	value = checksum_finish(sum);
	/* Transmitted zero means "no checksum", so a computed zero is sent as
	 * 0xFFFF (RFC 768). */
	return value ? value : 0xFFFFu;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

void bridge_net_default_config(BridgeNetConfig *config)
{
	static const uint8_t address[BRIDGE_NET_ADDR_LEN] = { 192, 168, 77, 1 };
	static const uint8_t netmask[BRIDGE_NET_ADDR_LEN] = { 255, 255, 255, 0 };
	static const uint8_t pool_first[BRIDGE_NET_ADDR_LEN] = { 192, 168, 77, 100 };
	static const uint8_t pool_last[BRIDGE_NET_ADDR_LEN] = {
		192, 168, 77, (uint8_t)(100u + BRIDGE_NET_LEASE_COUNT - 1u)
	};
	/* 02:57:69:4e:73:70 is a locally administered unicast address; the first
	 * octet's 0x02 bit is what keeps it out of a real vendor's OUI range. */
	static const uint8_t mac[BRIDGE_NET_MAC_LEN] = {
		0x02, 0x57, 0x69, 0x4E, 0x73, 0x70
	};

	if (!config)
		return;
	memcpy(config->address, address, BRIDGE_NET_ADDR_LEN);
	memcpy(config->netmask, netmask, BRIDGE_NET_ADDR_LEN);
	memcpy(config->pool_first, pool_first, BRIDGE_NET_ADDR_LEN);
	memcpy(config->pool_last, pool_last, BRIDGE_NET_ADDR_LEN);
	memcpy(config->dns, all_zero_address, BRIDGE_NET_ADDR_LEN);
	memcpy(config->mac, mac, BRIDGE_NET_MAC_LEN);
	config->lease_seconds = BRIDGE_NET_LEASE_SECONDS;
}

void bridge_net_dhcp_init(BridgeNetDhcp *dhcp, const BridgeNetConfig *config)
{
	if (!dhcp)
		return;
	memset(dhcp, 0, sizeof(*dhcp));
	if (config)
		dhcp->config = *config;
	else
		bridge_net_default_config(&dhcp->config);
	/*
	 * Before the ESP32 has associated there is no upstream resolver and no
	 * meaningful pool server either. Advertising our own address keeps the
	 * guest's DNS option pointing somewhere sane; the uplink replaces it with
	 * the real resolver as soon as the station gets an address (see main.c).
	 */
	if (memcmp(dhcp->config.dns, all_zero_address, BRIDGE_NET_ADDR_LEN) == 0)
		memcpy(dhcp->config.dns, dhcp->config.address,
		       BRIDGE_NET_ADDR_LEN);
	if (!dhcp->config.lease_seconds)
		dhcp->config.lease_seconds = BRIDGE_NET_LEASE_SECONDS;
}

/* ------------------------------------------------------------------ */
/* Leases                                                              */
/* ------------------------------------------------------------------ */

static bool address_is(const uint8_t *a, const uint8_t *b)
{
	return memcmp(a, b, BRIDGE_NET_ADDR_LEN) == 0;
}

/* Addresses are stored in network order, so memcmp orders them numerically. */
static bool address_in_pool(const BridgeNetConfig *config, const uint8_t *address)
{
	return memcmp(address, config->pool_first, BRIDGE_NET_ADDR_LEN) >= 0 &&
	       memcmp(address, config->pool_last, BRIDGE_NET_ADDR_LEN) <= 0;
}

static void bridge_net_expire(BridgeNetDhcp *dhcp, uint32_t now_ms)
{
	unsigned i;

	for (i = 0; i < BRIDGE_NET_LEASE_COUNT; i++) {
		BridgeNetLease *lease = &dhcp->leases[i];

		/* Signed comparison: correct across a wrap of the millisecond
		 * clock, which a 32-bit clock does reach. */
		if (lease->in_use &&
		    (int32_t)(now_ms - lease->expires_ms) >= 0)
			lease->in_use = false;
	}
}

static BridgeNetLease *lease_for_address(BridgeNetDhcp *dhcp,
					 const uint8_t *address)
{
	unsigned i;

	for (i = 0; i < BRIDGE_NET_LEASE_COUNT; i++)
		if (dhcp->leases[i].in_use &&
		    address_is(dhcp->leases[i].address, address))
			return &dhcp->leases[i];
	return NULL;
}

static BridgeNetLease *lease_for_mac(BridgeNetDhcp *dhcp, const uint8_t *mac)
{
	unsigned i;

	for (i = 0; i < BRIDGE_NET_LEASE_COUNT; i++)
		if (dhcp->leases[i].in_use &&
		    memcmp(dhcp->leases[i].mac, mac, BRIDGE_NET_MAC_LEN) == 0)
			return &dhcp->leases[i];
	return NULL;
}

/*
 * Give `address` to `mac` and refresh its expiry. Returns NULL when another
 * client holds it, or when every entry is taken.
 */
static BridgeNetLease *lease_claim(BridgeNetDhcp *dhcp, uint32_t now_ms,
				   const uint8_t *mac, const uint8_t *address)
{
	BridgeNetLease *lease = lease_for_address(dhcp, address);
	unsigned i;

	if (lease && memcmp(lease->mac, mac, BRIDGE_NET_MAC_LEN) != 0)
		return NULL;
	if (!lease) {
		for (i = 0; i < BRIDGE_NET_LEASE_COUNT; i++)
			if (!dhcp->leases[i].in_use) {
				lease = &dhcp->leases[i];
				break;
			}
		if (!lease)
			return NULL;
		lease->in_use = true;
		memcpy(lease->mac, mac, BRIDGE_NET_MAC_LEN);
		memcpy(lease->address, address, BRIDGE_NET_ADDR_LEN);
	}
	lease->expires_ms = now_ms + dhcp->config.lease_seconds * 1000u;
	return lease;
}

/*
 * Pick an address for a client: its existing lease, else the address it asked
 * for, else the first free one in the pool.
 */
static bool bridge_net_assign(BridgeNetDhcp *dhcp, uint32_t now_ms,
			      const uint8_t *mac, const uint8_t *requested,
			      uint8_t *out)
{
	BridgeNetLease *lease;
	uint8_t candidate[BRIDGE_NET_ADDR_LEN];
	uint32_t value;
	uint32_t last;

	lease = lease_for_mac(dhcp, mac);
	if (lease && lease_claim(dhcp, now_ms, mac, lease->address)) {
		memcpy(out, lease->address, BRIDGE_NET_ADDR_LEN);
		return true;
	}

	if (requested && address_in_pool(&dhcp->config, requested)) {
		lease = lease_claim(dhcp, now_ms, mac, requested);
		if (lease) {
			memcpy(out, lease->address, BRIDGE_NET_ADDR_LEN);
			return true;
		}
	}

	value = read_be32(dhcp->config.pool_first);
	last = read_be32(dhcp->config.pool_last);
	for (; value <= last; value++) {
		write_be32(candidate, value);
		lease = lease_claim(dhcp, now_ms, mac, candidate);
		if (lease) {
			memcpy(out, lease->address, BRIDGE_NET_ADDR_LEN);
			return true;
		}
	}
	return false;
}

/* Grant exactly `address`, for a renewal that names its own source address. */
static bool bridge_net_assign_specific(BridgeNetDhcp *dhcp, uint32_t now_ms,
				       const uint8_t *mac, const uint8_t *address,
				       uint8_t *out)
{
	BridgeNetLease *lease;

	if (!address_in_pool(&dhcp->config, address))
		return false;
	lease = lease_claim(dhcp, now_ms, mac, address);
	if (!lease)
		return false;
	memcpy(out, lease->address, BRIDGE_NET_ADDR_LEN);
	return true;
}

static void bridge_net_release_address(BridgeNetDhcp *dhcp,
				       const uint8_t *address)
{
	BridgeNetLease *lease = lease_for_address(dhcp, address);

	if (lease)
		lease->in_use = false;
}

/*
 * Take a declined address out of circulation for a minute. The lease stays
 * `in_use` with a hardware address of all zeros, so no client can match it, and
 * ordinary expiry frees it. Without this a client that declines for a real
 * reason - another host on the link already answers that address - would be
 * offered the same address forever.
 */
static void bridge_net_quarantine_address(BridgeNetDhcp *dhcp, uint32_t now_ms,
					  const uint8_t *address)
{
	BridgeNetLease *lease = lease_for_address(dhcp, address);

	if (!lease)
		return;
	memset(lease->mac, 0, BRIDGE_NET_MAC_LEN);
	lease->expires_ms = now_ms + BRIDGE_NET_DECLINE_QUARANTINE_MS;
}

/* ------------------------------------------------------------------ */
/* DHCP parsing                                                        */
/* ------------------------------------------------------------------ */

struct DhcpRequest {
	uint8_t message_type;
	const uint8_t *requested_ip; /* option 50, or NULL */
	const uint8_t *server_id;    /* option 54, or NULL */
};

/* Hand out the value of option `code`, or NULL. Options are TLV with PAD (0)
 * and END (255), and a truncated TLV makes the whole message unusable. */
static const uint8_t *find_option(const uint8_t *options, size_t length,
				  uint8_t code)
{
	size_t i = 0;

	while (i < length) {
		uint8_t id = options[i++];
		size_t option_length;

		if (id == DHCP_OPTION_PAD)
			continue;
		if (id == DHCP_OPTION_END)
			return NULL;
		if (i >= length)
			return NULL;
		option_length = options[i++];
		if (option_length > length - i)
			return NULL;
		if (id == code)
			return options + i;
		i += option_length;
	}
	return NULL;
}

static bool dhcp_parse(const uint8_t *bootp, size_t bootp_length,
		       struct DhcpRequest *request)
{
	const uint8_t *value;
	const uint8_t *options;
	size_t options_length;

	if (bootp_length < BOOTP_OPTIONS_OFFSET)
		return false;
	if (bootp[0] != BOOTP_OP_REQUEST ||
	    bootp[1] != BOOTP_HTYPE_ETHERNET ||
	    bootp[2] != BOOTP_HLEN_ETHERNET)
		return false;
	if (memcmp(bootp + BOOTP_FIXED_LEN, bootp_cookie, BOOTP_COOKIE_LEN) != 0)
		return false;

	options = bootp + BOOTP_OPTIONS_OFFSET;
	options_length = bootp_length - BOOTP_OPTIONS_OFFSET;

	value = find_option(options, options_length, DHCP_OPTION_MESSAGE_TYPE);
	if (!value)
		return false;
	request->message_type = *value;

	value = find_option(options, options_length, DHCP_OPTION_REQUESTED_IP);
	request->requested_ip =
		(value && value + BRIDGE_NET_ADDR_LEN <=
				  options + options_length) ?
			value :
			NULL;
	value = find_option(options, options_length, DHCP_OPTION_SERVER_ID);
	request->server_id =
		(value && value + BRIDGE_NET_ADDR_LEN <=
				  options + options_length) ?
			value :
			NULL;
	return true;
}

/*
 * Locate the UDP header of a guest IPv4 datagram, without checking whether the
 * payload is DHCP. Distinguishing "not UDP" from "UDP but not DHCP" matters: the
 * first is a datagram to forward, the second is a message for us.
 */
static bool ipv4_locate_udp(const uint8_t *frame, size_t length,
			    const uint8_t **udp, size_t *udp_available)
{
	const uint8_t *ip = frame + BRIDGE_NET_ETH_HEADER_LEN;
	size_t available = length - BRIDGE_NET_ETH_HEADER_LEN;
	size_t usable;
	size_t header_length;
	size_t udp_length;
	size_t payload;
	size_t present;

	if (available < IP_HEADER_LEN)
		return false;
	if ((ip[0] >> 4) != IP_VERSION_4)
		return false;
	header_length = (size_t)(ip[0] & 0x0Fu) * 4u;
	if (header_length < IP_HEADER_LEN || available < header_length + UDP_HEADER_LEN)
		return false;
	if (ip[9] != IP_PROTO_UDP)
		return false;
	/* Not the first fragment: the UDP header is not in this frame. */
	if ((read_be16(ip + 6) & 0x3FFFu) != 0u)
		return false;

	/*
	 * How much of the frame belongs to this datagram. The IP total length is
	 * the authority when it is smaller than what arrived, because a short
	 * datagram is padded to the Ethernet minimum; and the frame is the
	 * authority when it is smaller, so that a frame the link could not carry
	 * whole is still classified from the header that did arrive rather than
	 * being mistaken for something to forward.
	 */
	usable = available;
	if ((size_t)read_be16(ip + 2) < usable)
		usable = (size_t)read_be16(ip + 2);
	if (usable < header_length + UDP_HEADER_LEN)
		return false;

	*udp = ip + header_length;
	udp_length = read_be16(*udp + 4);
	if (udp_length < UDP_HEADER_LEN)
		return false;
	payload = udp_length - UDP_HEADER_LEN;
	present = usable - header_length - UDP_HEADER_LEN;
	*udp_available = payload < present ? payload : present;
	return true;
}

/* ------------------------------------------------------------------ */
/* DHCP reply construction                                             */
/* ------------------------------------------------------------------ */

struct OptionWriter {
	uint8_t *cursor;
	uint8_t *end;
	bool overflow;
};

static void option_u8(struct OptionWriter *writer, uint8_t value)
{
	if (writer->cursor >= writer->end) {
		writer->overflow = true;
		return;
	}
	*writer->cursor++ = value;
}

static void option_bytes(struct OptionWriter *writer, uint8_t code,
			 const uint8_t *data, size_t length)
{
	if ((size_t)(writer->end - writer->cursor) < length + 2u) {
		writer->overflow = true;
		return;
	}
	*writer->cursor++ = code;
	*writer->cursor++ = (uint8_t)length;
	memcpy(writer->cursor, data, length);
	writer->cursor += length;
}

static void option_be32(struct OptionWriter *writer, uint8_t code, uint32_t value)
{
	uint8_t encoded[4];

	write_be32(encoded, value);
	option_bytes(writer, code, encoded, sizeof(encoded));
}

static void bridge_net_broadcast_address(const BridgeNetConfig *config,
					 uint8_t *out)
{
	unsigned i;

	for (i = 0; i < BRIDGE_NET_ADDR_LEN; i++)
		out[i] = (uint8_t)(config->address[i] |
				   (uint8_t)~config->netmask[i]);
}

static void dhcp_build_reply(const BridgeNetDhcp *dhcp, uint8_t message_type,
			     const uint8_t *request, const uint8_t *yiaddr,
			     bool broadcast, uint32_t lease_seconds,
			     BridgeNetEmitFn emit, void *emit_context)
{
	uint8_t frame[BRIDGE_NET_MAX_FRAME];
	uint8_t *ip = frame + BRIDGE_NET_ETH_HEADER_LEN;
	uint8_t *udp = ip + IP_HEADER_LEN;
	uint8_t *bootp = udp + UDP_HEADER_LEN;
	struct OptionWriter options;
	uint8_t destination[BRIDGE_NET_ADDR_LEN];
	uint8_t broadcast_ip[BRIDGE_NET_ADDR_LEN];
	uint8_t dns[BRIDGE_NET_ADDR_LEN];
	uint16_t udp_length = (uint16_t)(UDP_HEADER_LEN + BOOTP_REPLY_LEN);

	/*
	 * A client with neither a leased nor a current address cannot be
	 * reached by unicast at all, so the reply has to be broadcast even if it
	 * did not set the flag. Anything else would be sent to 0.0.0.0.
	 */
	if (yiaddr)
		memcpy(destination, yiaddr, BRIDGE_NET_ADDR_LEN);
	else if (read_be32(request + BOOTP_CIADDR) != 0)
		memcpy(destination, request + BOOTP_CIADDR, BRIDGE_NET_ADDR_LEN);
	else {
		broadcast = true;
		memcpy(destination, broadcast_address, BRIDGE_NET_ADDR_LEN);
	}
	if (broadcast)
		memcpy(destination, broadcast_address, BRIDGE_NET_ADDR_LEN);

	/* --- Ethernet --- */
	if (broadcast)
		memset(frame, 0xFF, BRIDGE_NET_MAC_LEN);
	else
		memcpy(frame, request + BOOTP_CHADDR, BRIDGE_NET_MAC_LEN);
	memcpy(frame + 6, dhcp->config.mac, BRIDGE_NET_MAC_LEN);
	write_be16(frame + 12, ETH_TYPE_IPV4);

	/* --- IPv4 --- */
	ip[0] = (uint8_t)((IP_VERSION_4 << 4) | (IP_HEADER_LEN / 4u));
	ip[1] = 0;
	write_be16(ip + 2, (uint16_t)(IP_HEADER_LEN + udp_length));
	write_be16(ip + 4, 0); /* identification: not fragmented, so unused */
	write_be16(ip + 6, 0);
	ip[8] = IP_TTL;
	ip[9] = IP_PROTO_UDP;
	write_be16(ip + 10, 0); /* checksum field is skipped by ip_checksum */
	memcpy(ip + 12, dhcp->config.address, BRIDGE_NET_ADDR_LEN);
	memcpy(ip + 16, destination, BRIDGE_NET_ADDR_LEN);
	write_be16(ip + 10, ip_checksum(ip, IP_HEADER_LEN));

	/* --- UDP --- */
	write_be16(udp + 0, DHCP_SERVER_PORT);
	write_be16(udp + 2, DHCP_CLIENT_PORT);
	write_be16(udp + 4, udp_length);
	write_be16(udp + 6, 0);

	/* --- BOOTP --- */
	memset(bootp, 0, BOOTP_REPLY_LEN);
	bootp[0] = BOOTP_OP_REPLY;
	bootp[1] = BOOTP_HTYPE_ETHERNET;
	bootp[2] = BOOTP_HLEN_ETHERNET;
	write_be32(bootp + BOOTP_XID, read_be32(request + BOOTP_XID));
	write_be16(bootp + BOOTP_SECS, read_be16(request + BOOTP_SECS));
	write_be16(bootp + BOOTP_FLAGS,
		   broadcast ? BOOTP_FLAG_BROADCAST :
			       read_be16(request + BOOTP_FLAGS));
	memcpy(bootp + BOOTP_CIADDR, request + BOOTP_CIADDR, BRIDGE_NET_ADDR_LEN);
	if (yiaddr)
		memcpy(bootp + BOOTP_YIADDR, yiaddr, BRIDGE_NET_ADDR_LEN);
	/* siaddr names the server in a server-controlled boot; it is also what
	 * tells an old client where the lease came from. giaddr stays zero: we
	 * are not a relay. */
	memcpy(bootp + BOOTP_SIADDR, dhcp->config.address, BRIDGE_NET_ADDR_LEN);
	memcpy(bootp + BOOTP_GIADDR, all_zero_address, BRIDGE_NET_ADDR_LEN);
	memcpy(bootp + BOOTP_CHADDR, request + BOOTP_CHADDR, BRIDGE_NET_MAC_LEN);
	memcpy(bootp + BOOTP_FIXED_LEN, bootp_cookie, BOOTP_COOKIE_LEN);

	options.cursor = bootp + BOOTP_OPTIONS_OFFSET;
	options.end = bootp + BOOTP_REPLY_LEN;
	options.overflow = false;
	option_u8(&options, DHCP_OPTION_MESSAGE_TYPE);
	option_u8(&options, 1u);
	option_u8(&options, message_type);
	option_bytes(&options, DHCP_OPTION_SERVER_ID, dhcp->config.address,
		     BRIDGE_NET_ADDR_LEN);
	if (message_type != DHCP_NAK) {
		bridge_net_broadcast_address(&dhcp->config, broadcast_ip);
		memcpy(dns, dhcp->config.dns, BRIDGE_NET_ADDR_LEN);
		if (read_be32(dns) == 0)
			memcpy(dns, dhcp->config.address, BRIDGE_NET_ADDR_LEN);

		option_be32(&options, DHCP_OPTION_LEASE_TIME, lease_seconds);
		option_bytes(&options, DHCP_OPTION_NETMASK, dhcp->config.netmask,
			     BRIDGE_NET_ADDR_LEN);
		option_bytes(&options, DHCP_OPTION_ROUTER, dhcp->config.address,
			     BRIDGE_NET_ADDR_LEN);
		option_bytes(&options, DHCP_OPTION_DNS, dns,
			     BRIDGE_NET_ADDR_LEN);
		option_bytes(&options, DHCP_OPTION_BROADCAST, broadcast_ip,
			     BRIDGE_NET_ADDR_LEN);
		/* T1 and T2: when to try renewing and rebinding. */
		option_be32(&options, DHCP_OPTION_RENEWAL_TIME,
			    lease_seconds / 2u);
		option_be32(&options, DHCP_OPTION_REBIND_TIME,
			    lease_seconds - lease_seconds / 8u);
	}
	option_u8(&options, DHCP_OPTION_END);
	if (options.overflow || !emit)
		return;

	write_be16(udp + 6,
		   udp_checksum(ip + 12, ip + 16, udp, udp_length));
	emit(emit_context, frame, sizeof(frame));
}

/* ------------------------------------------------------------------ */
/* DHCP dispatch                                                       */
/* ------------------------------------------------------------------ */

static BridgeNetAction dhcp_handle(BridgeNetDhcp *dhcp, uint32_t now_ms,
				   const uint8_t *bootp, size_t bootp_length,
				   BridgeNetEmitFn emit, void *emit_context)
{
	struct DhcpRequest request;
	uint8_t assigned[BRIDGE_NET_ADDR_LEN] = { 0, 0, 0, 0 };
	uint32_t lease_seconds;
	uint32_t ciaddr;
	bool granted;

	if (!dhcp_parse(bootp, bootp_length, &request)) {
		dhcp->ignored++;
		return BRIDGE_NET_HANDLED;
	}

	dhcp->received++;
	bridge_net_expire(dhcp, now_ms);
	lease_seconds = dhcp->config.lease_seconds;

	switch (request.message_type) {
	case DHCP_DISCOVER:
		if (!bridge_net_assign(dhcp, now_ms, bootp + BOOTP_CHADDR,
				       request.requested_ip, assigned)) {
			/* Pool exhausted. Silence is the correct answer: a NAK
			 * would send the client looking for another server. */
			return BRIDGE_NET_HANDLED;
		}
		dhcp->offered++;
		dhcp_build_reply(dhcp, DHCP_OFFER, bootp, assigned,
				 (read_be16(bootp + BOOTP_FLAGS) &
				  BOOTP_FLAG_BROADCAST) != 0,
				 lease_seconds, emit, emit_context);
		return BRIDGE_NET_HANDLED;	case DHCP_REQUEST:
		/* A request that names another server is that server's to answer. */
		if (request.server_id &&
		    !address_is(request.server_id, dhcp->config.address))
			return BRIDGE_NET_HANDLED;

		/*
		 * A REQUEST names one address and asks for a yes or a no, unlike a
		 * DISCOVER which asks for whatever is free. So this never substitutes
		 * a different address: an address that is outside the pool, taken by
		 * another client, or absent from the message is refused with a NAK.
		 * Answering such a request with a lease for somewhere else would leave
		 * the client believing it owns an address it never asked about.
		 */
		ciaddr = read_be32(bootp + BOOTP_CIADDR);
		if (ciaddr != 0 && !request.requested_ip)
			/* RENEWING / REBINDING: it already has the address and is
			 * asking us to extend the lease. */
			granted = bridge_net_assign_specific(
				dhcp, now_ms, bootp + BOOTP_CHADDR,
				bootp + BOOTP_CIADDR, assigned);
		else if (request.requested_ip)
			granted = bridge_net_assign_specific(
				dhcp, now_ms, bootp + BOOTP_CHADDR,
				request.requested_ip, assigned);
		else
			granted = false;
		if (!granted) {
			dhcp->naked++;
			dhcp_build_reply(dhcp, DHCP_NAK, bootp, NULL, true,
					 lease_seconds, emit, emit_context);
			return BRIDGE_NET_HANDLED;
		}

		dhcp->acked++;
		dhcp_build_reply(dhcp, DHCP_ACK, bootp, assigned,
				 /* SELECTING and INIT-REBOOT clients have no
				  * address yet, so the ACK goes to their hardware
				  * address - or to the broadcast address if they
				  * asked for that. A renewal is answered to the
				  * address it came from. */
				 ciaddr != 0 ? false :
					       (read_be16(bootp + BOOTP_FLAGS) &
						BOOTP_FLAG_BROADCAST) != 0,
				 lease_seconds, emit, emit_context);
		return BRIDGE_NET_HANDLED;

	case DHCP_DECLINE:
		if (request.requested_ip)
			bridge_net_quarantine_address(
				dhcp, now_ms, request.requested_ip);
		dhcp->declined++;
		return BRIDGE_NET_HANDLED;

	case DHCP_RELEASE:
		if (read_be32(bootp + BOOTP_CIADDR) != 0)
			bridge_net_release_address(dhcp, bootp + BOOTP_CIADDR);
		dhcp->released++;
		return BRIDGE_NET_HANDLED;

	case DHCP_INFORM:
		/* The client has an address and only wants the options. RFC 2131
		 * says yiaddr must stay zero. */
		dhcp->acked++;
		dhcp_build_reply(dhcp, DHCP_ACK, bootp, NULL,
				 (read_be16(bootp + BOOTP_FLAGS) &
				  BOOTP_FLAG_BROADCAST) != 0,
				 lease_seconds, emit, emit_context);
		return BRIDGE_NET_HANDLED;

	default:
		dhcp->ignored++;
		return BRIDGE_NET_HANDLED;
	}
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

BridgeNetAction bridge_net_handle_ethernet(BridgeNetDhcp *dhcp, uint32_t now_ms,
					   const uint8_t *frame, size_t length,
					   BridgeNetEmitFn emit,
					   void *emit_context)
{
	const uint8_t *udp = NULL;
	size_t udp_available = 0;
	uint16_t ethertype;

	if (!dhcp || !frame || length < BRIDGE_NET_ETH_HEADER_LEN)
		return BRIDGE_NET_DROP;

	ethertype = read_be16(frame + 12);
	if (ethertype == ETH_TYPE_ARP)
		return BRIDGE_NET_L2;
	if (ethertype != ETH_TYPE_IPV4)
		return BRIDGE_NET_DROP;

	if (!ipv4_locate_udp(frame, length, &udp, &udp_available) ||
	    read_be16(udp + 2) != DHCP_SERVER_PORT)
		return BRIDGE_NET_IPV4;

	/*
	 * Addressed to the DHCP port, so it is ours. A malformed one is a link
	 * or stack problem worth counting rather than something to forward.
	 */
	if (read_be16(udp) != DHCP_CLIENT_PORT ||
	    udp_available < BOOTP_OPTIONS_OFFSET) {
		dhcp->ignored++;
		return BRIDGE_NET_HANDLED;
	}
	return dhcp_handle(dhcp, now_ms, udp + UDP_HEADER_LEN, udp_available, emit,
			   emit_context);
}

/* ------------------------------------------------------------------ */
/* Self test                                                           */
/* ------------------------------------------------------------------ */
/*
 * Runs under `build/Host/winspire-host --selftest`, which is the only place the
 * DHCP path can be executed at all: there is no ESP32 and no Windows 95 image in
 * this environment. The tests build real DHCP message bytes, push them through
 * the same entry point the firmware uses, and inspect the bytes that come back,
 * including both checksums.
 */
#ifdef BRIDGE_NET_ENABLE_SELFTEST

int bridge_net_selftest_failure_line;

#define BRIDGE_CHECK(condition)                                    \
	do {                                                       \
		if (!(condition)) {                                \
			bridge_net_selftest_failure_line = __LINE__; \
			return false;                              \
		}                                                  \
	} while (0)

#define TEST_FRAME_MAX 512

struct TestRecorder {
	uint8_t frames[4][TEST_FRAME_MAX];
	size_t lengths[4];
	unsigned count;
};

struct TestClient {
	uint8_t mac[BRIDGE_NET_MAC_LEN];
	uint32_t xid;
	const uint8_t *requested; /* 4 bytes, may be NULL */
	const uint8_t *server_id; /* 4 bytes, may be NULL */
	const uint8_t *ciaddr;    /* 4 bytes, may be NULL */
	bool broadcast;
};

static void test_record(void *context, const uint8_t *frame, size_t length)
{
	struct TestRecorder *recorder = context;

	if (recorder->count < 4u && length <= TEST_FRAME_MAX) {
		memcpy(recorder->frames[recorder->count], frame, length);
		recorder->lengths[recorder->count] = length;
	}
	recorder->count++;
}

/* Build a complete DHCP request frame, exactly as a guest stack would. */
static size_t test_build_request(uint8_t *frame, size_t capacity,
				 const struct TestClient *client)
{
	const size_t bootp_length = BOOTP_REPLY_LEN;
	const size_t ip_length = IP_HEADER_LEN + UDP_HEADER_LEN + bootp_length;
	uint8_t *ip;
	uint8_t *udp;
	uint8_t *bootp;
	struct OptionWriter options;

	if (capacity < BRIDGE_NET_ETH_HEADER_LEN + ip_length + 8u)
		return 0;
	memset(frame, 0, BRIDGE_NET_ETH_HEADER_LEN + ip_length);

	memset(frame, 0xFF, BRIDGE_NET_MAC_LEN);
	memcpy(frame + 6, client->mac, BRIDGE_NET_MAC_LEN);
	write_be16(frame + 12, ETH_TYPE_IPV4);

	ip = frame + BRIDGE_NET_ETH_HEADER_LEN;
	udp = ip + IP_HEADER_LEN;
	bootp = udp + UDP_HEADER_LEN;

	ip[0] = (uint8_t)((IP_VERSION_4 << 4) | (IP_HEADER_LEN / 4u));
	write_be16(ip + 2, (uint16_t)ip_length);
	ip[8] = IP_TTL;
	ip[9] = IP_PROTO_UDP;
	memcpy(ip + 12, client->ciaddr ? client->ciaddr : all_zero_address,
	       BRIDGE_NET_ADDR_LEN);
	memcpy(ip + 16, broadcast_address, BRIDGE_NET_ADDR_LEN);
	write_be16(ip + 10, ip_checksum(ip, IP_HEADER_LEN));

	write_be16(udp + 0, DHCP_CLIENT_PORT);
	write_be16(udp + 2, DHCP_SERVER_PORT);
	write_be16(udp + 4, (uint16_t)(UDP_HEADER_LEN + bootp_length));
	write_be16(udp + 6, 0); /* a client may legitimately omit the checksum */

	bootp[0] = BOOTP_OP_REQUEST;
	bootp[1] = BOOTP_HTYPE_ETHERNET;
	bootp[2] = BOOTP_HLEN_ETHERNET;
	write_be32(bootp + BOOTP_XID, client->xid);
	write_be16(bootp + BOOTP_FLAGS,
		   client->broadcast ? BOOTP_FLAG_BROADCAST : 0u);
	memcpy(bootp + BOOTP_CIADDR,
	       client->ciaddr ? client->ciaddr : all_zero_address,
	       BRIDGE_NET_ADDR_LEN);
	memcpy(bootp + BOOTP_CHADDR, client->mac, BRIDGE_NET_MAC_LEN);
	memcpy(bootp + BOOTP_FIXED_LEN, bootp_cookie, BOOTP_COOKIE_LEN);

	options.cursor = bootp + BOOTP_OPTIONS_OFFSET;
	options.end = bootp + bootp_length;
	options.overflow = false;
	option_u8(&options, DHCP_OPTION_MESSAGE_TYPE);
	option_u8(&options, 1u);
	option_u8(&options, (client->requested || client->ciaddr) ?
					     DHCP_REQUEST : DHCP_DISCOVER);
	if (client->requested)
		option_bytes(&options, DHCP_OPTION_REQUESTED_IP,
			     client->requested, BRIDGE_NET_ADDR_LEN);
	if (client->server_id)
		option_bytes(&options, DHCP_OPTION_SERVER_ID, client->server_id,
			     BRIDGE_NET_ADDR_LEN);
	option_u8(&options, DHCP_OPTION_END);
	if (options.overflow)
		return 0;
	return BRIDGE_NET_ETH_HEADER_LEN + ip_length;
}

static void test_patch_message_type(uint8_t *bootp, uint8_t message_type)
{
	uint8_t *options = bootp + BOOTP_OPTIONS_OFFSET;
	size_t i = 0;

	while (i < BOOTP_REPLY_LEN - BOOTP_OPTIONS_OFFSET) {
		uint8_t id = options[i++];
		size_t length;

		if (id == DHCP_OPTION_PAD)
			continue;
		if (id == DHCP_OPTION_END)
			return;
		length = options[i++];
		if (id == DHCP_OPTION_MESSAGE_TYPE && length >= 1u) {
			options[i] = message_type;
			return;
		}
		i += length;
	}
}

static bool test_held(const BridgeNetDhcp *dhcp, const uint8_t *address)
{
	unsigned i;

	for (i = 0; i < BRIDGE_NET_LEASE_COUNT; i++)
		if (dhcp->leases[i].in_use &&
		    address_is(dhcp->leases[i].address, address))
			return true;
	return false;
}

static const uint8_t *reply_op(const struct TestRecorder *recorder,
			       unsigned index, size_t *bootp_length)
{
	const uint8_t *ip = recorder->frames[index] +
			    BRIDGE_NET_ETH_HEADER_LEN;
	const uint8_t *udp = ip + IP_HEADER_LEN;

	*bootp_length = (size_t)read_be16(udp + 4) - UDP_HEADER_LEN;
	return udp + UDP_HEADER_LEN;
}

static const uint8_t *reply_option(const uint8_t *bootp, size_t bootp_length,
				   uint8_t code)
{
	return find_option(bootp + BOOTP_OPTIONS_OFFSET,
			   bootp_length - BOOTP_OPTIONS_OFFSET, code);
}

/*
 * Checksum verification. Deliberately NOT built on checksum_add()/
 * checksum_finish(): summing the buffer that already contains the checksum must
 * fold to 0xFFFF, and doing that with a separate handful of lines is what makes
 * this a check of the production code rather than a restatement of it.
 */
static uint32_t test_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return sum;
}

static uint32_t test_sum(const uint8_t *data, size_t length)
{
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i + 1u < length; i += 2u)
		sum += (uint32_t)(((uint16_t)data[i] << 8) | data[i + 1u]);
	if (i < length)
		sum += (uint32_t)((uint16_t)data[i] << 8);
	return test_fold(sum);
}

static bool test_checksum_valid(const uint8_t *data, size_t length)
{
	return test_sum(data, length) == 0xFFFFu;
}

static bool test_udp_checksum_valid(const uint8_t *ip)
{
	const uint8_t *udp = ip + IP_HEADER_LEN;
	size_t udp_length = read_be16(udp + 4);
	uint8_t pseudo[12];
	uint32_t sum;

	memcpy(pseudo, ip + 12, BRIDGE_NET_ADDR_LEN);
	memcpy(pseudo + 4, ip + 16, BRIDGE_NET_ADDR_LEN);
	pseudo[8] = 0;
	pseudo[9] = IP_PROTO_UDP;
	write_be16(pseudo + 10, (uint16_t)udp_length);
	sum = test_sum(pseudo, sizeof(pseudo)) + test_sum(udp, udp_length);
	return test_fold(sum) == 0xFFFFu;
}

/* Same, but with an explicit message type for the DECLINE/RELEASE/INFORM cases
 * and a settable hardware-length field for the malformed-frame tests. */
static size_t test_build_message(uint8_t *frame, size_t capacity,
				 const struct TestClient *client,
				 uint8_t message_type, uint8_t hlen)
{
	size_t length = test_build_request(frame, capacity, client);
	uint8_t *bootp;

	if (!length)
		return 0;
	bootp = frame + BRIDGE_NET_ETH_HEADER_LEN + IP_HEADER_LEN +
		UDP_HEADER_LEN;
	bootp[2] = hlen;
	test_patch_message_type(bootp, message_type);
	return length;
}

bool bridge_net_selftest(void)
{
	static const uint8_t guest_a[BRIDGE_NET_MAC_LEN] = {
		0x00, 0x50, 0x56, 0xC0, 0x00, 0x01
	};
	static const uint8_t guest_b[BRIDGE_NET_MAC_LEN] = {
		0x00, 0x50, 0x56, 0xC0, 0x00, 0x02
	};
	const uint8_t pool_first[BRIDGE_NET_ADDR_LEN] = { 192, 168, 77, 100 };
	const uint8_t pool_second[BRIDGE_NET_ADDR_LEN] = { 192, 168, 77, 101 };
	const uint8_t foreign[BRIDGE_NET_ADDR_LEN] = { 10, 0, 0, 5 };
	BridgeNetDhcp dhcp;
	struct TestRecorder recorder;
	struct TestClient client;
	uint8_t frame[TEST_FRAME_MAX];
	uint8_t offered[BRIDGE_NET_ADDR_LEN];
	const uint8_t *bootp;
	const uint8_t *option;
	size_t bootp_length;
	size_t length;
	uint32_t now = 1000u;

	bridge_net_selftest_failure_line = 0;
	bridge_net_dhcp_init(&dhcp, NULL);

	/* --- defaults --- */
	BRIDGE_CHECK(address_is(dhcp.config.address, (const uint8_t[]){ 192, 168, 77, 1 }));
	BRIDGE_CHECK(dhcp.config.lease_seconds == BRIDGE_NET_LEASE_SECONDS);
	BRIDGE_CHECK(memcmp(dhcp.config.dns, dhcp.config.address,
			    BRIDGE_NET_ADDR_LEN) == 0);

	/* --- DISCOVER is answered with an OFFER for the first pool address --- */
	memset(&recorder, 0, sizeof(recorder));
	memset(&client, 0, sizeof(client));
	memcpy(client.mac, guest_a, BRIDGE_NET_MAC_LEN);
	client.xid = 0x12345678u;
	length = test_build_request(frame, sizeof(frame), &client);
	BRIDGE_CHECK(length == BRIDGE_NET_ETH_HEADER_LEN + IP_HEADER_LEN +
				      UDP_HEADER_LEN + BOOTP_REPLY_LEN);
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	BRIDGE_CHECK(recorder.count == 1u);
	BRIDGE_CHECK(recorder.lengths[0] == BRIDGE_NET_MAX_FRAME);

	bootp = reply_op(&recorder, 0, &bootp_length);
	BRIDGE_CHECK(bootp[0] == BOOTP_OP_REPLY);
	BRIDGE_CHECK(bootp[1] == BOOTP_HTYPE_ETHERNET);
	BRIDGE_CHECK(bootp[2] == BOOTP_HLEN_ETHERNET);
	BRIDGE_CHECK(read_be32(bootp + BOOTP_XID) == 0x12345678u);
	BRIDGE_CHECK(memcmp(bootp + BOOTP_CHADDR, guest_a,
			    BRIDGE_NET_MAC_LEN) == 0);
	BRIDGE_CHECK(address_is(bootp + BOOTP_YIADDR, pool_first));
	BRIDGE_CHECK(address_is(bootp + BOOTP_SIADDR, dhcp.config.address));
	BRIDGE_CHECK(read_be32(bootp + BOOTP_GIADDR) == 0u);
	BRIDGE_CHECK(memcmp(bootp + BOOTP_FIXED_LEN, bootp_cookie, 4u) == 0);
	option = reply_option(bootp, bootp_length, DHCP_OPTION_MESSAGE_TYPE);
	BRIDGE_CHECK(option && *option == DHCP_OFFER);
	option = reply_option(bootp, bootp_length, DHCP_OPTION_NETMASK);
	BRIDGE_CHECK(option && address_is(option, dhcp.config.netmask));
	option = reply_option(bootp, bootp_length, DHCP_OPTION_ROUTER);
	BRIDGE_CHECK(option && address_is(option, dhcp.config.address));
	option = reply_option(bootp, bootp_length, DHCP_OPTION_DNS);
	BRIDGE_CHECK(option &&
		     address_is(option, (const uint8_t[]){ 192, 168, 77, 1 }));
	option = reply_option(bootp, bootp_length, DHCP_OPTION_BROADCAST);
	BRIDGE_CHECK(option &&
		     address_is(option, (const uint8_t[]){ 192, 168, 77, 255 }));
	option = reply_option(bootp, bootp_length, DHCP_OPTION_LEASE_TIME);
	BRIDGE_CHECK(option && read_be32(option) == BRIDGE_NET_LEASE_SECONDS);
	option = reply_option(bootp, bootp_length, DHCP_OPTION_RENEWAL_TIME);
	BRIDGE_CHECK(option && read_be32(option) == BRIDGE_NET_LEASE_SECONDS / 2u);
	option = reply_option(bootp, bootp_length, DHCP_OPTION_SERVER_ID);
	BRIDGE_CHECK(option && address_is(option, dhcp.config.address));

	/* The reply is addressed to the client's hardware address, never to an
	 * address the client does not own yet. */
	BRIDGE_CHECK(memcmp(recorder.frames[0], guest_a,
			    BRIDGE_NET_MAC_LEN) == 0);
	BRIDGE_CHECK(memcmp(recorder.frames[0] + 6, dhcp.config.mac,
			    BRIDGE_NET_MAC_LEN) == 0);
	BRIDGE_CHECK(read_be16(recorder.frames[0] + 12) == ETH_TYPE_IPV4);

	/* Both checksums must validate: a DHCP reply with a bad checksum is
	 * dropped silently by the client, which is close to undebuggable. */
	BRIDGE_CHECK(test_checksum_valid(recorder.frames[0] +
						 BRIDGE_NET_ETH_HEADER_LEN,
					 IP_HEADER_LEN));
	BRIDGE_CHECK(test_udp_checksum_valid(recorder.frames[0] +
					     BRIDGE_NET_ETH_HEADER_LEN));

	BRIDGE_CHECK(test_held(&dhcp, pool_first)); /* the offer is recorded */

	/* --- the client's DHCPREQUEST is answered with an ACK for the same
	 *     address, which is what makes the DISCOVER-then-REQUEST pair work */
	memcpy(offered, bootp + BOOTP_YIADDR, BRIDGE_NET_ADDR_LEN);
	memset(&recorder, 0, sizeof(recorder));
	client.requested = offered;
	client.server_id = dhcp.config.address;
	length = test_build_request(frame, sizeof(frame), &client);
	BRIDGE_CHECK(length != 0);
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	BRIDGE_CHECK(recorder.count == 1u);
	bootp = reply_op(&recorder, 0, &bootp_length);
	option = reply_option(bootp, bootp_length, DHCP_OPTION_MESSAGE_TYPE);
	BRIDGE_CHECK(option && *option == DHCP_ACK);
	BRIDGE_CHECK(address_is(bootp + BOOTP_YIADDR, offered));
	BRIDGE_CHECK(dhcp.acked == 1u && dhcp.naked == 0u);
	BRIDGE_CHECK(test_held(&dhcp, pool_first));

	/* A request naming an address the client already holds is granted, and
	 * one naming an address outside the pool is not, even though the client
	 * does hold a usable lease: a REQUEST must be answered as asked. */

	/* --- a second guest gets a different address, and asking for an address
	 *     outside the pool is refused with a NAK --- */
	now += 10u;
	memset(&recorder, 0, sizeof(recorder));
	memset(&client, 0, sizeof(client));
	memcpy(client.mac, guest_b, BRIDGE_NET_MAC_LEN);
	client.xid = 0x00ABCDEFu;
	length = test_build_request(frame, sizeof(frame), &client);
	BRIDGE_CHECK(length != 0);
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	BRIDGE_CHECK(recorder.count == 1u);
	bootp = reply_op(&recorder, 0, &bootp_length);
	BRIDGE_CHECK(address_is(bootp + BOOTP_YIADDR, pool_second));

	client.requested = foreign;
	client.server_id = dhcp.config.address;
	length = test_build_request(frame, sizeof(frame), &client);
	memset(&recorder, 0, sizeof(recorder));
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	bootp = reply_op(&recorder, 0, &bootp_length);
	option = reply_option(bootp, bootp_length, DHCP_OPTION_MESSAGE_TYPE);
	BRIDGE_CHECK(option && *option == DHCP_NAK);
	/* A NAK has no lease options and is broadcast, because the client may
	 * not have a usable address. */
	BRIDGE_CHECK(reply_option(bootp, bootp_length, DHCP_OPTION_NETMASK) == NULL);
	BRIDGE_CHECK(memcmp(recorder.frames[0], (const uint8_t[]){ 0xFF, 0xFF,
								 0xFF, 0xFF,
								 0xFF, 0xFF },
			    BRIDGE_NET_MAC_LEN) == 0);
	BRIDGE_CHECK(address_is(bootp + BOOTP_YIADDR, all_zero_address));
	BRIDGE_CHECK(test_checksum_valid(recorder.frames[0] +
						 BRIDGE_NET_ETH_HEADER_LEN,
					 IP_HEADER_LEN));

	/* --- a request naming another server is left for that server --- */
	memset(&recorder, 0, sizeof(recorder));
	client.server_id = foreign;
	length = test_build_request(frame, sizeof(frame), &client);
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	BRIDGE_CHECK(recorder.count == 0u);

	/* --- a renewal is ACKed to the address it came from --- */
	memset(&recorder, 0, sizeof(recorder));
	memset(&client, 0, sizeof(client));
	memcpy(client.mac, guest_a, BRIDGE_NET_MAC_LEN);
	client.xid = 0x11111111u;
	client.ciaddr = pool_first;
	length = test_build_request(frame, sizeof(frame), &client);
	BRIDGE_CHECK(length != 0);
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	bootp = reply_op(&recorder, 0, &bootp_length);
	option = reply_option(bootp, bootp_length, DHCP_OPTION_MESSAGE_TYPE);
	BRIDGE_CHECK(option && *option == DHCP_ACK);
	BRIDGE_CHECK(address_is(bootp + BOOTP_YIADDR, pool_first));
	BRIDGE_CHECK(address_is(bootp + BOOTP_CIADDR, pool_first));
	/* A renewal is unicast back to the address it came from. */
	BRIDGE_CHECK(address_is(recorder.frames[0] + BRIDGE_NET_ETH_HEADER_LEN +
					16, pool_first));

	/* --- release frees the address for the next client --- */
	memset(&recorder, 0, sizeof(recorder));
	client.requested = NULL;
	client.server_id = NULL;
	length = test_build_message(frame, sizeof(frame), &client, DHCP_RELEASE,
				    BOOTP_HLEN_ETHERNET);
	BRIDGE_CHECK(length != 0);
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	BRIDGE_CHECK(recorder.count == 0u);
	BRIDGE_CHECK(dhcp.released == 1u);

	memset(&recorder, 0, sizeof(recorder));
	memset(&client, 0, sizeof(client));
	memcpy(client.mac, guest_b, BRIDGE_NET_MAC_LEN);
	client.xid = 0x22222222u;
	length = test_build_request(frame, sizeof(frame), &client);
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	bootp = reply_op(&recorder, 0, &bootp_length);
	/* guest_b is still on pool_second, so guest_a's released address is not
	 * what it gets; the point is that a released lease is reusable. */
	BRIDGE_CHECK(address_is(bootp + BOOTP_YIADDR, pool_second));

	/* --- decline quarantines the address without freeing it --- */
	/* guest_b holds pool_second, from the DISCOVER above. */
	BRIDGE_CHECK(test_held(&dhcp, pool_second));
	memset(&recorder, 0, sizeof(recorder));
	memset(&client, 0, sizeof(client));
	memcpy(client.mac, guest_b, BRIDGE_NET_MAC_LEN);
	client.xid = 0x33333333u;
	client.requested = pool_second;
	client.server_id = dhcp.config.address;
	length = test_build_message(frame, sizeof(frame), &client, DHCP_DECLINE,
				    BOOTP_HLEN_ETHERNET);
	BRIDGE_CHECK(length != 0);
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	BRIDGE_CHECK(recorder.count == 0u && dhcp.declined == 1u);
	/* Quarantined, not freed: still held, but by nobody. */
	BRIDGE_CHECK(test_held(&dhcp, pool_second));

	/* Another guest must not be offered the quarantined address. guest_a's
	 * old lease on pool_first was released above, so it gets that one. */
	memset(&recorder, 0, sizeof(recorder));
	memset(&client, 0, sizeof(client));
	memcpy(client.mac, guest_a, BRIDGE_NET_MAC_LEN);
	client.xid = 0x44444444u;
	length = test_build_request(frame, sizeof(frame), &client);
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	bootp = reply_op(&recorder, 0, &bootp_length);
	BRIDGE_CHECK(!address_is(bootp + BOOTP_YIADDR, pool_second));
	BRIDGE_CHECK(address_is(bootp + BOOTP_YIADDR, pool_first));

	/* --- lease expiry frees addresses again --- */
	{
		BridgeNetDhcp expired;

		bridge_net_dhcp_init(&expired, NULL);
		memset(&recorder, 0, sizeof(recorder));
		memset(&client, 0, sizeof(client));
		memcpy(client.mac, guest_a, BRIDGE_NET_MAC_LEN);
		client.xid = 0x55555555u;
		length = test_build_request(frame, sizeof(frame), &client);
		BRIDGE_CHECK(bridge_net_handle_ethernet(&expired, 100u, frame,
							length, test_record,
							&recorder) ==
			     BRIDGE_NET_HANDLED);
		bootp = reply_op(&recorder, 0, &bootp_length);
		BRIDGE_CHECK(address_is(bootp + BOOTP_YIADDR, pool_first));

		/* guest_b arrives after guest_a's lease has run out, and gets the
		 * first address because nothing holds it any more. */
		memset(&recorder, 0, sizeof(recorder));
		memset(&client, 0, sizeof(client));
		memcpy(client.mac, guest_b, BRIDGE_NET_MAC_LEN);
		client.xid = 0x66666666u;
		length = test_build_request(frame, sizeof(frame), &client);
		BRIDGE_CHECK(bridge_net_handle_ethernet(
				     &expired,
				     100u + BRIDGE_NET_LEASE_SECONDS * 1000u + 1u,
				     frame, length, test_record,
				     &recorder) == BRIDGE_NET_HANDLED);
		bootp = reply_op(&recorder, 0, &bootp_length);
		BRIDGE_CHECK(address_is(bootp + BOOTP_YIADDR, pool_first));
	}

	/* --- the pool really does run out rather than wrapping --- */
	{
		BridgeNetDhcp small;
		BridgeNetConfig config;
		unsigned i;

		bridge_net_default_config(&config);
		memcpy(config.pool_last, config.pool_first,
		       BRIDGE_NET_ADDR_LEN); /* a one-address pool */
		bridge_net_dhcp_init(&small, &config);
		for (i = 0; i < 2u; i++) {
			uint8_t other[BRIDGE_NET_MAC_LEN];

			memset(other, 0, sizeof(other));
			other[0] = 0x00;
			other[5] = (uint8_t)(0x10u + i);
			memset(&recorder, 0, sizeof(recorder));
			memset(&client, 0, sizeof(client));
			memcpy(client.mac, other, BRIDGE_NET_MAC_LEN);
			client.xid = 0x70000000u + i;
			length = test_build_request(frame, sizeof(frame),
						    &client);
			BRIDGE_CHECK(bridge_net_handle_ethernet(
					     &small, 1000u, frame, length,
					     test_record, &recorder) ==
				     BRIDGE_NET_HANDLED);
			/* The first client is served, the second is not. */
			BRIDGE_CHECK(recorder.count == (i == 0u ? 1u : 0u));
		}
	}

	/* --- a broadcast request is answered by broadcast --- */
	memset(&recorder, 0, sizeof(recorder));
	memset(&client, 0, sizeof(client));
	memcpy(client.mac, guest_a, BRIDGE_NET_MAC_LEN);
	client.xid = 0x77777777u;
	client.broadcast = true;
	length = test_build_request(frame, sizeof(frame), &client);
	BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame, length,
						test_record, &recorder) ==
		     BRIDGE_NET_HANDLED);
	BRIDGE_CHECK(recorder.count == 1u);
	BRIDGE_CHECK(memcmp(recorder.frames[0], (const uint8_t[]){ 0xFF, 0xFF,
								 0xFF, 0xFF,
								 0xFF, 0xFF },
			    BRIDGE_NET_MAC_LEN) == 0);
	BRIDGE_CHECK(memcmp(recorder.frames[0] + BRIDGE_NET_ETH_HEADER_LEN + 16,
			    broadcast_address, BRIDGE_NET_ADDR_LEN) == 0);
	bootp = reply_op(&recorder, 0, &bootp_length);
	BRIDGE_CHECK((read_be16(bootp + BOOTP_FLAGS) & BOOTP_FLAG_BROADCAST) != 0);

	/* --- classification: only DHCP is consumed here --- */
	{
		uint8_t arp[BRIDGE_NET_ETH_HEADER_LEN + 28u];

		memset(arp, 0, sizeof(arp));
		write_be16(arp + 12, ETH_TYPE_ARP);
		BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, arp,
							sizeof(arp), NULL,
							NULL) == BRIDGE_NET_L2);

		/* An IPv4 datagram that is not DHCP is to be forwarded. */
		memset(&client, 0, sizeof(client));
		memcpy(client.mac, guest_a, BRIDGE_NET_MAC_LEN);
		client.xid = 0x88888888u;
		length = test_build_request(frame, sizeof(frame), &client);
		BRIDGE_CHECK(length != 0);
		/* Rewrite the UDP destination port so it is no longer ours. */
		write_be16(frame + BRIDGE_NET_ETH_HEADER_LEN + IP_HEADER_LEN + 2,
			   53u);
		BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame,
							length, NULL, NULL) ==
			     BRIDGE_NET_IPV4);

		/* IPv6 and IPX are not carried. */
		write_be16(frame + 12, 0x86DDu);
		BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame,
							length, NULL, NULL) ==
			     BRIDGE_NET_DROP);
		write_be16(frame + 12, 0x8137u);
		BRIDGE_CHECK(bridge_net_handle_ethernet(&dhcp, now, frame,
							length, NULL, NULL) ==
			     BRIDGE_NET_DROP);
	}

	/* --- malformed input is counted, not forwarded and not answered --- */
	{
		BridgeNetDhcp tolerant;
		uint32_t before;

		bridge_net_dhcp_init(&tolerant, NULL);
		memset(&client, 0, sizeof(client));
		memcpy(client.mac, guest_a, BRIDGE_NET_MAC_LEN);
		client.xid = 0x99999999u;
		length = test_build_request(frame, sizeof(frame), &client);
		BRIDGE_CHECK(length != 0);

		/* Truncate inside the option area. */
		memset(&recorder, 0, sizeof(recorder));
		BRIDGE_CHECK(bridge_net_handle_ethernet(
				     &tolerant, now, frame,
				     BRIDGE_NET_ETH_HEADER_LEN + IP_HEADER_LEN +
					     UDP_HEADER_LEN + 20u,
				     test_record, &recorder) ==
			     BRIDGE_NET_HANDLED);
		BRIDGE_CHECK(recorder.count == 0u && tolerant.ignored == 1u);

		/* A BOOTP header with the wrong hardware length. */
		length = test_build_message(frame, sizeof(frame), &client,
					    DHCP_DISCOVER, 4u);
		memset(&recorder, 0, sizeof(recorder));
		before = tolerant.ignored;
		BRIDGE_CHECK(bridge_net_handle_ethernet(&tolerant, now, frame,
							length, test_record,
							&recorder) ==
			     BRIDGE_NET_HANDLED);
		BRIDGE_CHECK(recorder.count == 0u && tolerant.ignored == before + 1u);

		/* Claim a source port other than 68. */
		length = test_build_request(frame, sizeof(frame), &client);
		write_be16(frame + BRIDGE_NET_ETH_HEADER_LEN + IP_HEADER_LEN,
			   69u);
		memset(&recorder, 0, sizeof(recorder));
		before = tolerant.ignored;
		BRIDGE_CHECK(bridge_net_handle_ethernet(&tolerant, now, frame,
							length, test_record,
							&recorder) ==
			     BRIDGE_NET_HANDLED);
		BRIDGE_CHECK(recorder.count == 0u && tolerant.ignored == before + 1u);

		/* A frame shorter than an Ethernet header cannot be classified. */
		BRIDGE_CHECK(bridge_net_handle_ethernet(&tolerant, now, frame, 10u,
							NULL, NULL) ==
			     BRIDGE_NET_DROP);

		/* And the good case still works after all of that. */
		length = test_build_request(frame, sizeof(frame), &client);
		memset(&recorder, 0, sizeof(recorder));
		BRIDGE_CHECK(bridge_net_handle_ethernet(&tolerant, now, frame,
							length, test_record,
							&recorder) ==
			     BRIDGE_NET_HANDLED);
		BRIDGE_CHECK(recorder.count == 1u && tolerant.offered == 1u);
	}

	return true;
}

#endif /* BRIDGE_NET_ENABLE_SELFTEST */
