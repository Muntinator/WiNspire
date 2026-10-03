/*
 * bridge_net - the ESP32 half of the guest's network: a DHCP server and a
 * classifier for the frames the calculator sends up the cxlink link.
 *
 * WHY THIS FILE EXISTS SEPARATELY
 * -------------------------------
 * Everything here is freestanding (only <stdint.h>, <stdbool.h>, <stddef.h>,
 * <string.h>) and free of lwIP, ESP-IDF and Ndless, for one reason: the ESP32
 * firmware cannot be executed in the environment this tree is developed in, but
 * this code can. The DHCP server - the part that is easiest to get subtly wrong
 * and hardest to debug on a real Windows 95 guest - is therefore unit tested by
 * the host build (`make selftest`), which is the only executable verification
 * this project has. See bridge_net.c for the checks.
 *
 * WHERE IT SITS
 * -------------
 *   guest NIC (NE2000 in Windows 95)
 *        |  NET_TX over cxlink
 *        v
 *   bridge_net_handle_ethernet()        <- this file
 *        |  DHCP?          -> answer locally, emit a reply on the guest link
 *        |  ARP / other L2 -> BRIDGE_NET_L2, handed to lwIP
 *        |  IPv4           -> BRIDGE_NET_IPV4, forwarded and NATed by lwIP
 *        v
 *   ESP32 Wi-Fi station -> the access point -> the Internet
 *
 * The subnet below exists ONLY on the calculator link. It is never routed onto
 * Wi-Fi: lwIP's NAPT rewrites the guest's source address and port to the ESP32's
 * station address before anything leaves the radio, so the guest gets a normal
 * private LAN (a DHCP lease, its own TCP/IP stack, its own ARP) and the access
 * point only ever sees the ESP32. That is why the address range is unusual: it
 * only has to be unlikely to collide with the uplink's own subnet, because a
 * collision is the one case where routing between the two would misfire.
 *
 * The DHCP server answers from this file rather than through lwIP's own DHCP
 * server app, for a specific reason: lwIP would have to ARP for the address it
 * is about to hand out before it can deliver the offer, and the client does not
 * own that address yet. Addressing the reply to the client's hardware address -
 * which is what every DHCP server does and what this code does - removes that
 * whole failure mode.
 */
#ifndef BRIDGE_NET_H
#define BRIDGE_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Guest-link addressing                                               */
/* ------------------------------------------------------------------ */

#define BRIDGE_NET_ETH_HEADER_LEN 14u
#define BRIDGE_NET_ADDR_LEN 4
#define BRIDGE_NET_MAC_LEN 6

/* One guest with one NIC needs one lease; the rest is headroom for a reboot
 * that changes source port or for a second guest frontend. */
#define BRIDGE_NET_LEASE_COUNT 8u

/* Long enough not to churn, short enough that a changed client comes back. */
#define BRIDGE_NET_LEASE_SECONDS 3600u

typedef struct {
	uint8_t address[BRIDGE_NET_ADDR_LEN]; /* this router, as the guest sees it */
	uint8_t netmask[BRIDGE_NET_ADDR_LEN];
	uint8_t pool_first[BRIDGE_NET_ADDR_LEN];
	uint8_t pool_last[BRIDGE_NET_ADDR_LEN];
	/* Option 6. Learned from the uplink's own DHCP client, so it can be
	 * 0.0.0.0 before the ESP32 has associated; the reply substitutes the
	 * router address in that case (see bridge_net_dhcp_init). */
	uint8_t dns[BRIDGE_NET_ADDR_LEN];
	uint8_t mac[BRIDGE_NET_MAC_LEN];
	uint32_t lease_seconds;
} BridgeNetConfig;

/* Fill config with the defaults described above. */
void bridge_net_default_config(BridgeNetConfig *config);

typedef struct {
	uint8_t mac[BRIDGE_NET_MAC_LEN];
	uint8_t address[BRIDGE_NET_ADDR_LEN];
	uint32_t expires_ms;
	bool in_use;
} BridgeNetLease;

typedef struct {
	BridgeNetConfig config;
	BridgeNetLease leases[BRIDGE_NET_LEASE_COUNT];
	/*
	 * Counters. A guest that cannot get an address is one of the few failures
	 * that is invisible from the calculator side, so these are reported back
	 * through the link (see NETWORK_ARCHITECTURE.md).
	 */
	uint32_t received;  /* DHCP messages accepted from the guest */
	uint32_t offered;
	uint32_t acked;
	uint32_t naked;
	uint32_t released;
	uint32_t declined;
	uint32_t ignored;   /* looked like DHCP but unusable: bad length, hlen, ... */
} BridgeNetDhcp;

void bridge_net_dhcp_init(BridgeNetDhcp *dhcp, const BridgeNetConfig *config);

/* ------------------------------------------------------------------ */
/* Frame intake                                                        */
/* ------------------------------------------------------------------ */

/*
 * Called to transmit one frame this code produced on the guest link (a DHCP
 * reply). Kept as a callback so bridge_net.c owns no transport, which is what
 * lets the self test drive it with an in-memory recorder.
 */
typedef void (*BridgeNetEmitFn)(void *context, const uint8_t *frame,
				size_t length);

typedef enum {
	BRIDGE_NET_DROP = 0, /* nothing to do: not traffic we carry */
	BRIDGE_NET_HANDLED,  /* answered here; a reply may have been emitted */
	BRIDGE_NET_IPV4,     /* guest IPv4 datagram: forward it */
	BRIDGE_NET_L2,       /* ARP and other link-layer traffic: hand to lwIP */
} BridgeNetAction;

/*
 * Classify one Ethernet frame received from the guest, and answer it if it is a
 * DHCP message. `now_ms` is a monotonic millisecond clock (any 32-bit origin;
 * only differences are used) that drives lease expiry. `emit` may be NULL when
 * the caller only wants classification.
 */
BridgeNetAction bridge_net_handle_ethernet(BridgeNetDhcp *dhcp, uint32_t now_ms,
					   const uint8_t *frame, size_t length,
					   BridgeNetEmitFn emit,
					   void *emit_context);

/*
 * Built only when the caller asks for it, so the firmware and the .tns do not
 * carry the test.
 */
#ifdef BRIDGE_NET_ENABLE_SELFTEST
bool bridge_net_selftest(void);
/* Line number of the check that failed, or 0. Set by bridge_net_selftest(). */
extern int bridge_net_selftest_failure_line;
#endif

#endif /* BRIDGE_NET_H */
