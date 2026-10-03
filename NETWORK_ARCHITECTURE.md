# NETWORK_ARCHITECTURE.md — Windows 95 TCP/IP over the ESP32

## 1. Principle

Windows 95 must see **a normal emulated Ethernet adapter** and run **its own
TCP/IP stack**. The ESP32 transports frames and must not become the guest's
network stack. The guest must not need to know the ESP32 exists.

## 2. Chosen NIC: NE2000

The adapter is an emulated NE2000 (`source/winspire/ne2000.c`, inherited from
QEMU's implementation). It was already in the tree and it is the right choice:

- Windows 95 ships an in-box NE2000 driver, so no driver work is needed and no
  guest-side software is added.
- It is a real ISA card model: I/O ports at `0x300`, IRQ 9/10, 16 KiB of packet
  RAM, a MAC address in PROM, TX/RX ring buffers, and the NE2000's
  command/page/boundary register model.
- It is already wired to the 8259 PIC through the existing `set_irq` callback, so
  the IRQ path into Windows 95 is the standard one.

### 2.1 The backend is a seam, and that is the whole integration

`ne2000.c` already abstracts the *outside world* behind a two-function interface:

```c
struct VC {
    void (*send_packet)(void *vc, uint8_t *buf, int size);
    void (*step)(NE2000State *s);
};
```

and chooses an implementation in `net_open()` (tuntap → slirp → null). This port
adds a fourth option, `USE_CXLINK`, in the same style:

```c
case USE_CXLINK:  send_packet -> cxlink_net_send_frame()
                  step        -> drain up to 8 frames via cxlink_net_poll_frame()
```

**The guest-facing card is byte-for-byte unchanged.** That is exactly the
requirement "the interface presented to the guest must remain the same Ethernet
adapter, and the guest must not need to know that the ESP32 is providing the
physical network connection."

`CXLINK_RX_BATCH` bounds how many received frames are delivered per step. A
bounded batch is the difference between "the ESP32 is slow" and "the CPU stalls":
the emulator never processes an unbounded number of frames in one `pc_step`.

## 3. Frame path

```
Windows 95 TCP/IP
    v
Win95 NE2000 driver
    v
emulated NE2000 (ne2000.c)  --- IRQ via PIC --> guest
    v  send_packet
cxlink_net_send_frame()  (cxlink.c)
    |  NET_TX frame, ACK requested, sequence++, one outstanding at a time
    v
cxlink framing + CRC-16/CCITT + sequence
    v
UART 115200 8N1  ->  ESP32
    v
net_handle_tx()  (source/esp32/main/main.c, runs on the link task)
    |  NET_TX retry of the sequence already accepted? -> counted, not forwarded
    v
bridge_net_handle_ethernet()   (source/winspire/bridge_net.c, shared source)
    |  DHCP (UDP 67)       -> answered here; reply goes back out NET_RX
    |  other link layer    -> BRIDGE_NET_L2   -> pbuf -> cx_netif.input()
    |  IPv4                -> BRIDGE_NET_IPV4 -> net_forward_ipv4()
    v
lwIP on the ESP32 (guest netif "cx" -> station netif -> radio)
    |  ARP and ICMP for the gateway address answered by lwIP
    |  IP_FORWARD + IPV4_NAPT: route it, then rewrite source address/port
    v
Wi-Fi station  ->  access point  ->  LAN / Internet

   ... and the reverse:
radio -> station netif -> ip4_input -> NAPT un-translates the destination
      -> routed to "cx" -> cx_netif_linkoutput() -> cxlink_send_net_rx()
      -> NET_RX frame -> cxlink_net_queue_push() -> ne2000_receive()
      -> guest driver -> Windows 95 TCP/IP
```

## 4. Reliable vs unreliable channels, and why

| Channel | Messages | Delivery | Rationale |
|---|---|---|---|
| Management | `HELLO`, `VERSION`, `PING`/`PONG`, `STATUS`, `RESET` | best effort | Idempotent; a lost ping is harmless |
| Audio | `AUDIO_CONFIG`, `AUDIO_DATA`, `AUDIO_FLUSH`, `AUDIO_STATUS` | **best effort, no ACK** | A late sample is worthless; ACKs would halve effective audio bandwidth |
| Network | `NET_TX`, `NET_RX` | **acknowledged, ordered** | Dropping an Ethernet frame silently breaks TCP; the guest must not have to detect link loss |
| Network state | `NET_CONFIG`, `NET_STATUS` | best effort, repeated | A lost report is superseded 2 s later; neither changes guest hardware |

The brief's requirement — *"Do not require an acknowledgement for every audio
block"* — is met by making audio explicitly unacknowledged, while network frames
get the reliability they need.

**Window of one.** The reliable channel keeps a single frame outstanding, with a
40 ms retransmit timer and up to 6 retries before the link is declared down.
Ethernet is already retried end-to-end by TCP, the link is a single serial line
with a small bounded round trip, and a window of one keeps the state machine
small enough to be obviously correct. It also gives a clean, predictable
backpressure signal: `cxlink_net_send_frame()` returns `false` while a frame is
outstanding, and the NE2000 reports a lost transmit — exactly what a real card
does under congestion. Windows 95's stack handles that.

`NET_TX` retries set `CXLINK_FLAG_RETRY`, so the ESP32 can count duplicates
rather than forwarding a frame twice. The firmware remembers the last accepted
`NET_TX` sequence: a `RETRY` carrying that sequence is counted in
`net_duplicate_frames` and dropped, while a `RETRY` with an unknown sequence is
forwarded, because that is what a restarted calculator looks like. Duplicates
are reported to the calculator in `CxlinkNetStatus.link_errors`, next to
transport-level errors, since both mean "the link is losing frames".

## 5. Integrity and resynchronisation

- **CRC-16/CCITT-FALSE** over the type byte through the payload. Chosen over
  CRC-32 because it is one small table (or table-free) and is ample for a short
  framed link with retransmission. The sync bytes are deliberately excluded from
  the CRC: corrupting them should not produce a "good" frame.
- The decoder is a **byte-at-a-time state machine** so it can be driven straight
  from a UART interrupt, and it **resynchronises** rather than trusting its own
  state: a length field above `CXLINK_MAX_PAYLOAD` is treated as desynchronisation
  and the decoder returns to hunting for `0x5A 0xA5`. CRC failures and format
  failures are counted separately (`frames_dropped_crc`,
  `frames_dropped_format`) so a marginal link is visible rather than mysterious.
- Sequence numbers are **per stream** (management, audio, network-TX,
  network-RX), as the brief specifies ("use sequence numbers where ordering
  matters"). Audio loss is detectable without being retried.

## 6. The honest problem: stock ESP-IDF cannot bridge raw 802.11

The brief anticipated this:

> "If raw Ethernet bridging is technically impossible through the available
> ESP32/CX connection, implement the closest transparent architecture while
> keeping the guest-facing NIC unchanged."

**It is not possible with stock ESP-IDF.**

- `esp_wifi_80211_tx()` only injects frames in AP or ESP-NOW modes. An ESP32
  associated with an existing access point cannot transmit arbitrary layer-2
  frames onto that AP.
- Promiscuous RX only delivers frames on the current channel with the current
  filter settings; it cannot see traffic addressed to other stations.

So a transparent 802.11 L2 bridge — the guest's fake MAC appearing on the wire —
is out of reach. Two architectures are actually available; the firmware
implements the first:

### Option A — ESP32 as a NAT router (**implemented**)

The ESP32 runs lwIP, joins an existing access point as a station, runs a DHCP
server for the guest, and forwards the **IP payloads** of the guest's Ethernet
frames with source NAT. Windows 95 keeps its own TCP/IP stack, does its own DHCP
against the ESP32, and does its own ARP; the ESP32 is simply the router it is
plugged into. There is no SoftAP in this build: the calculator's link *is* the
guest's wire, so an AP would only add a second radio hop that carries one client.

This preserves everything that matters:
- The guest-facing NIC is unchanged (still an NE2000).
- TCP/IP is still terminated inside Windows 95 — the ESP32 never terminates a
  guest TCP connection, it forwards IP packets (NAT), which is what a router
  does.
- DHCP/DNS/LAN/Internet all work in the normal way, because to the guest the
  ESP32 is a normal gateway.

What is implemented, specifically:

- `net_forward_ipv4()` in `source/esp32/main/main.c` is the entry point: it
  checks the IPv4 version nibble, copies the frame into a pbuf and hands it to
  the guest netif's input path.
- The guest netif is a raw lwIP `struct netif` named `cx`, built inside
  `esp_netif_tcpip_exec()`, with `etharp_output` and a `linkoutput` that copies
the pbuf chain (Ethernet header included) into a `CXLINK_MAX_PAYLOAD` buffer and
emits one `NET_RX` frame. Its MTU is `CXLINK_MAX_PAYLOAD - 14` = 1486, so any
datagram from the uplink that would not fit a link frame is fragmented by lwIP
instead of being dropped. It is deliberately **not** the default netif.
- The DHCP server, the frame classifier and the guest subnet live in
  `source/winspire/bridge_net.c` — shared source, freestanding (no lwIP, no
  ESP-IDF, no Ndless), and executed by the host build. See §6.2.
- lwIP's own forwarding and source translation do the routing.
  `CONFIG_LWIP_IP_FORWARD` and `CONFIG_LWIP_IPV4_NAPT` are enabled in
  `source/esp32/sdkconfig.defaults` and in the tracked `sdkconfig`; the firmware
  has a compile-time `#error` if either is missing, because without them the
  binary would build and then silently fail to reach the network.
- NAPT is armed on the **guest** netif only (`ip_napt_enable_netif()`), never on
  the station netif. lwIP applies NAPT when the *input* netif has `napt` set:
  for an outbound packet that is the guest netif, and for a reply arriving on the
  radio `ip4_input()` calls `ip_napt_recv()` to translate the destination back.
  Setting it on both ends would stop the two halves matching.
- NAPT state is only ever changed from the link task via
  `esp_netif_tcpip_exec()`, because mutating the netif list and the NAPT tables is
  not safe from an event handler context. The Wi-Fi event handlers do nothing
  except set two flags; `net_service()` performs the blocking calls.

### Option B — standalone SoftAP, no upstream

If no Internet access is needed, the ESP32 can instead be a self-contained AP:
the guest connects to it, and `esp_wifi_80211_tx()` can transmit raw frames to
associated stations. This gives genuine L2 behaviour between the guest and other
stations on the ESP32's own BSS, with no NAT anywhere. It does not provide
Internet access, and it is not implemented here — `wifi_setup()` puts the radio
in station mode.

### 6.1 The guest subnet

| Role | Address |
|---|---|
| Router / gateway / DNS | **192.168.77.1** |
| Netmask | 255.255.255.0 |
| DHCP pool | 192.168.77.100 – 192.168.77.107 (8 leases) |
| Hardware address of the ESP32's guest-side NIC | `02:57:69:4e:73:70` |
| Lease time | 3600 s |

This subnet exists **only** on the UART link. It is never routed onto Wi-Fi:
NAPT rewrites the guest's source address and port to the ESP32's station address
before anything leaves the radio, so the access point only ever sees the ESP32.
That is also why the range is unusual — it only has to be unlikely to collide
with the uplink's own subnet, since a collision is the one case where routing
between the two would misfire.

### 6.2 The DHCP server, and why it is hand-written

lwIP ships a DHCP server app, and this firmware does not use it. lwIP's server
would have to ARP for the address it is about to hand out before it can deliver
the offer, and the client does not own that address yet. Addressing the reply to
the client's hardware address — what every DHCP server does — removes that whole
failure mode, so the server is written out in `bridge_net.c` instead.

It handles DISCOVER/OFFER, REQUEST/ACK/NAK, DECLINE, RELEASE and INFORM, records
the lease at DISCOVER time so a lost reply is recoverable, quarantines a
declined address for 60 s, NAKs a REQUEST that names an address outside the pool
or held by someone else, and builds a complete Ethernet+IPv4+UDP+BOOTP reply with
validated IP and UDP checksums (300-byte BOOTP payload).

Because it is freestanding, it is also the only part of the firmware that can be
executed without hardware, and it is:

```
$ build/Host/winspire-host --selftest
cxlink self test: PASS (codec, ack/retry, audio framing, corruption recovery, reset)
bridge_net self test: PASS (DHCP offer/ack/nak, lease expiry, release, decline
                       quarantine, checksums, frame routing)
```

The self test found three real defects while it was being written (an IP checksum
that skipped four bytes instead of two; a REQUEST naming an unused address being
silently re-pointed at another lease; a frame padded to the Ethernet minimum
being mis-parsed). What it cannot do is prove the guest accepts the replies —
that needs Windows 95 on hardware.

### 6.3 Provisioning the uplink — implemented

The ESP32 has no console, no config file and no flash write. The access point's
SSID and password are typed into the calculator's own configuration file and
pushed over the link, which is the right place for them: the calculator is the
bridge's only console, and a password in build configuration is a password in
source control.

The whole path, end to end:

| Step | Where |
|---|---|
| User writes `[network] ssid` / `password` | `winspire.ini` on the calculator (template: `source/winspire/native.ini.tns`) |
| Parsed into `PCConfig.wifi_ssid` / `.wifi_password` | `parse_conf_ini()` in `source/winspire/pc.c` |
| Validated, refused with a message box if unusable | `cxlink_wifi_check()` — shared source |
| Sent as `NET_CONFIG{ssid,password}` once the bridge answers | `cxlink_net_provision()` + `cxlink_poll()` in `source/winspire/cxlink.c` |
| Validated again, applied to the station interface | `net_apply_uplink()` in `source/esp32/main/main.c` |
| Confirmed by reporting `CXLINK_NET_FLAG_CREDENTIALS` | `netsend_config()`, every 2 s |
| Retransmission stops; the uplink is used for the guest's traffic | `cxlink.c` |

The exchange is deliberately boring, because it has to work over a best-effort
channel with no user in front of the bridge:

- **Retransmission until confirmed.** The calculator re-sends every 2 s until the
  bridge reports holding the credentials. A lost frame costs one retry.
- **Recovery after a bridge restart is automatic.** The bridge keeps credentials
  in RAM only (`WIFI_STORAGE_RAM`, no flash write), so after a power cycle it
  reports "none" and the calculator puts them back on the wire. The frontend is
  not involved: the only thing it ever did was call `cxlink_net_provision()`
  once.
- **Retransmissions are idempotent.** The bridge compares both fields of the pair
  it is using and ignores an identical one, so a retry never causes a
  re-association. (A password change with an unchanged SSID *is* a change, which
  is why both are compared and not just the SSID.)
- **Both ends validate with the same function.** An SSID is 1–31 characters and a
  password is empty (open network) or 8–63. The limits are one byte below
  ESP-IDF's own 32/64-byte fields because those fields must stay NUL-terminated:
  accepting a 32-character SSID would mean the Wi-Fi stack reading a
  non-terminated string or silently joining a truncated network name. A refusal
  is a sentence in the calculator's message box, not a mystery.
- **A wrong password is distinguishable from a missing access point.** The bridge
  maps the disconnect reason (`WIFI_REASON_AUTH_FAIL`, the handshake timeouts,
  MIC failure, association failure) onto `CXLINK_NET_STATE_REJECTED` and reports
  it, so the calculator can say "the access point refused the credentials"
  instead of leaving the user guessing.
- **Secrets stay in RAM.** The password is never logged, never written to a file
  by the emulator, never stored in ESP32 flash, and it travels only over the
  wired dock link to a directly attached bridge. The calculator wipes its own
  copy when the emulator exits and the configuration is freed.

What this does **not** yet have: an on-device way to type a password, so
`winspire.ini` is edited on the calculator or in the `.tns` build. That is a
consequence of the host having no text-input API in this port, not of the
protocol, and it is the first thing to revisit if a settings screen is ever
added. Nothing here has been run against a real access point — see §7.

### 6.4 Liveness is not the uplink

`cxlink_link_up()` means *the bridge is answering*, and it is set by any frame
that decodes and passes CRC. It is deliberately not tied to
`CXLINK_NET_FLAG_UPLINK_UP`, which is the bridge's Wi-Fi association state:

- Audio must keep working when the bridge has no Wi-Fi at all, and the
  Sound Blaster path is gated on the link, not on the uplink.
- The guest's DHCP has to work *before* there is any uplink to forward to, which
  is exactly the case during provisioning.
- A bridge that genuinely goes away is detected separately: the calculator sends a
  keepalive `PING` after 2 s of silence and declares the link down after
  `CXLINK_BRIDGE_TIMEOUT_MS` (8 s), four times the firmware's report interval.

Conflating the two was a real bug in an earlier revision: a bridge with no
credentials reported "link down" forever, which muted the audio path as well.

## 7. What is implemented, precisely

| Piece | State |
|---|---|
| NE2000 `USE_CXLINK` backend in `ne2000.c` | **typechecked**, compiles in the CX and host builds |
| `cxlink_net_send_frame` / `cxlink_net_poll_frame` | **typechecked**; round trip byte-exact in `--selftest` |
| Frontend call site: `cxlink_poll()` every 1 ms (`service_io_bridge()`) | wired; **typechecked** for the CX, **verified running** on the host |
| Reliable channel: ACK/NACK, retransmit timer, retry cap, backpressure | **typechecked**; window-of-one and ACK matching checked in `--selftest` |
| Per-stream sequence numbers, CRC, resync | **typechecked**; corruption rejection and recovery after a bad frame checked in `--selftest` |
| RX frame queue (8 deep, drop-oldest) with bounded per-step batch | **typechecked** |
| DHCP server (`bridge_net.c`: OFFER/ACK/NAK/DECLINE/RELEASE/INFORM, expiry) | **verified** by `make selftest` on the host; never answered a real Windows 95 |
| Guest subnet, lease table, DHCP/BOOTP+UDP+IP checksums | **verified** by `make selftest` on the host |
| Frame classifier (`bridge_net_handle_ethernet()`) | **verified** by `make selftest` on the host (routing, padding, truncation) |
| ESP32 `NET_TX` / `NET_RX` / `NET_CONFIG` / `NET_STATUS` handling and framing | **builds clean** (ESP-IDF v5.5.5), **never run** |
| ESP32 `net_forward_ipv4()` + guest netif (`cx`) + `linkoutput` | **builds clean**, never carried a frame |
| NAPT arming (`ip_napt_enable_netif`, `esp_netif_tcpip_exec`) and DNS refresh | **builds clean**, never executed |
| Wi-Fi station uplink, reconnect on disconnect, credential apply | **builds clean**, never associated with an access point |
| `NET_TX` duplicate suppression on retry | **builds clean**, never exercised over the link |
| Calculator-side provisioning (`cxlink_net_provision`, `[network]` INI keys) | **verified** by `make selftest`; the host frontend shows the frame reaching a simulated bridge and being confirmed (`wifi provision` line) |
| Credential validation (`cxlink_wifi_check`, both ends) | **verified** by `make selftest` (10 boundary cases) |
| ESP32 `net_apply_uplink()`: apply, idempotence, disconnect-reason mapping | **builds clean**, never associated with an access point |
| Link liveness separated from uplink state, keepalive + 8 s timeout | **verified** in `make selftest`; **verified running** on the host |
| End-to-end networking on hardware | not attempted |

## 8. Bandwidth reality

Network traffic shares the 11 520 B/s link with 8-bit 8 kHz mono audio, which
uses ~8 620 B/s. That leaves roughly **2 900 B/s** for Ethernet — about 0.3% of
10 Mbit. Consequences, stated plainly:

- Ping, DNS, small HTTP requests, and text-mode browsing are realistic.
- Bulk transfer, media streaming, or Windows Update are not.
- Lowering the audio rate (via `AUDIO_CONFIG`) or muting audio frees the whole
  link for networking. `CXLINK_AUDIO_FLAG_MUTE` exists for exactly this.
- If 115200 is the true ceiling, the honest conclusion is that the original CX's
  dock UART is a telemetry-grade link, not a network link. The upside is that the
  guest-facing NIC does not change if the transport is later replaced — a faster
  transport only needs a new `CxlinkHal`.

The correct way to prioritise is measured, not guessed: `cxlink_get_status()`
reports `tx_frames`, `rx_frames`, `tx_dropped` and `link_errors`, and the ESP32
reports audio underruns. Watch those before changing anything.
