# cxlink ESP32 bridge firmware

The ESP32 half of the Ti-Nspire CX I/O bridge. It provides physical audio output
and a Wi-Fi network path for the PC emulator running on the calculator.

**This firmware is not a co-emulator.** The calculator runs the entire PC: CPU,
BIOS, VGA, IDE, Sound Blaster, and the NE2000 NIC. The ESP32 is a peripheral on
the other end of a serial link.

## Status

> **This firmware compiles clean and produces flashable images.** It has **never
> been run on hardware**: there is no ESP32 and no calculator in the environment
> where it was built. "Builds" and "works" are different claims and only the
> first one is made here.
>
> Verified: ESP-IDF v5.5.5, xtensa-esp-elf-gcc 14.2.0, cold build from an empty
> tree, no errors and no warnings, `cxlink-bridge.bin` 807 296 bytes (23% of the
> 1 MiB app partition left), `esptool image_info` checksum and validation hash
> both valid.

| Component | State |
|---|---|
| cxlink framing, CRC, sequences (`source/winspire/cxlink.c`, compiled in) | **builds clean**, and covered by `make selftest` on the host |
| DHCP server, guest subnet, frame classifier (`source/winspire/bridge_net.c`, compiled in) | **builds clean**, and **executed by `make selftest`** on the host |
| UART1 transport | **builds clean**, never run |
| I2S audio path + ring buffer + underrun accounting | **builds clean**, never run against a DAC |
| `PING`/`PONG`, `VERSION`, `AUDIO_CONFIG`, `AUDIO_FLUSH`, `AUDIO_STATUS`, `ERROR` | **builds clean**, never exchanged with a calculator |
| `NET_TX`/`NET_RX` framing and ACK | **builds clean**, never carried a real frame |
| `net_forward_ipv4()` -> guest lwIP netif | **builds clean**, never carried a frame |
| NAPT + forwarding (`CONFIG_LWIP_IP_FORWARD`, `CONFIG_LWIP_IPV4_NAPT`) | **builds clean**, never executed |
| Wi-Fi station uplink, reconnect, DNS refresh | **builds clean**, never associated |
| `NET_TX` duplicate suppression on retry | **builds clean**, never exercised |
| Uplink credentials from the calculator (`NET_CONFIG`) | **builds clean**; validated by shared code that `make selftest` exercises on the host, but never handed to a real radio |
| Disconnect-reason mapping ("refused" vs "not there yet") | **builds clean**, never seen a real disconnect |

## Build

```sh
bash source/esp32/build.sh
```

That writes `build/esp32/{bootloader/bootloader.bin, partition_table/partition-table.bin,
cxlink-bridge.bin}`, or `make esp32` does the same. `build.sh` explains the
one-time ESP-IDF setup in its header, including the two prerequisites that are
non-obvious on a minimal container (system cmake/ninja, and libusb for
install.sh's openocd version check, which otherwise aborts the install before it
ever creates the Python environment).

ESP-IDF v5.x is required: the I2S driver API used here is the v5
`driver/i2s_std.h` interface, not the legacy `driver/i2s.h`.

The image is built from the tracked `source/esp32/sdkconfig`, which is what a
build reads; `source/esp32/sdkconfig.defaults` records the settings that matter
(the two lwIP symbols above) so they are not lost if `sdkconfig` is regenerated.

To flash and watch it:

```sh
. ${IDF_PATH:-/tmp/esp-idf}/export.sh
idf.py -B build/esp32 -p /dev/ttyUSB0 flash monitor
```

### Why `cxlink.c` and `bridge_net.c` are compiled here

`main/CMakeLists.txt` compiles `source/winspire/cxlink.c` **and**
`source/winspire/bridge_net.c` into this firmware and puts `source/winspire` on
the include path, so the ESP32, the calculator and the host test build the **same
implementation**. Do not copy either: two copies will drift.

This was a real bug, caught by the first build: an earlier revision of
`main/CMakeLists.txt` added only the *include* path, so the firmware linked
against nothing and failed with undefined references to `cxlink_encode` and
`cxlink_decoder_push`. The protocol is split so the transport is the only
platform-specific part (`CxlinkHal` in `cxlink.h`); everything above it is shared
source. `bridge_net.c` depends on nothing at all (no lwIP, no ESP-IDF), which is
what lets `make selftest` run the DHCP server on a workstation.

`cxlink.c` needs `get_uticks()`, which `main.c` provides from
`esp_timer_get_time()` — it drives the reliable channel's retransmit timer and the
keepalive.

## Wiring

See [HARDWARE.md](../../HARDWARE.md) §3 for the connector pinout and cautions.

| TI-Nspire CX dock (J01) | ESP32 |
|---|---|
| pin 4 (Tx) | GPIO16 |
| pin 3 (Rx) | GPIO17 |
| pin 5 (GND) | GND |

| ESP32 | Audio |
|---|---|
| GPIO26 | I2S BCLK |
| GPIO25 | I2S WS |
| GPIO22 | I2S DOUT → DAC (MAX98357A, PCM5102, …) |

| ESP32 | Network |
|---|---|
| on-board radio | Wi-Fi |

Both UART ends are 3.3 V TTL: connect directly, never through RS-232 levels.
Tie grounds. Prefer a separate supply for the ESP32 over dock pin 2 (Vcc).

All pins are `#define`s at the top of `main/main.c`.

## Networking: this firmware is a NAT router

Stock ESP-IDF **cannot** bridge raw 802.11. `esp_wifi_80211_tx()` only injects
frames in AP or ESP-NOW mode, and promiscuous RX only sees frames on the current
channel — so an ESP32 associated with an existing AP cannot put the guest's MAC
on the wire. [NETWORK_ARCHITECTURE.md](../../NETWORK_ARCHITECTURE.md) §6 explains
this in full.

What this firmware does instead is the closest thing that keeps the guest-facing
NIC unchanged: it is **the router the guest's Ethernet cable is plugged into**.
The ESP32 joins your access point as a station, serves the guest subnet over the
link, and forwards the guest's IPv4 traffic with source NAT.

```
Windows 95 --(NE2000 -> cxlink -> UART)--> ESP32 --> Wi-Fi station --> LAN/Internet
            192.168.77.x        |            |
                                |            `-- DHCP server (bridge_net.c)
                                |            `-- lwIP: ARP, ICMP, IP_FORWARD + IPV4_NAPT
                                `-- the guest's own TCP/IP stack, end to end
```

The guest still runs its own TCP/IP stack, gets its address by DHCP, resolves its
own gateway by ARP, and never learns the ESP32 exists. This firmware does not
terminate a guest TCP connection; it forwards IP packets and translates source
addresses, which is what a router does. **The guest-facing NIC does not change**
— it is still an emulated NE2000 with Windows 95's own driver.

| Role | Address |
|---|---|
| Router / gateway / DNS | 192.168.77.1 |
| Netmask | 255.255.255.0 |
| DHCP pool | 192.168.77.100 – 192.168.77.107 |
| Guest-side MAC | `02:57:69:4e:73:70` |
| Lease | 3600 s |

The subnet exists only on the UART link; NAPT rewrites the guest's source address
and port before anything reaches the radio.

### Why the DHCP server is not lwIP's

`components/lwip/apps/dhcpserver/` exists, and this firmware does not use it.
lwIP's server would have to ARP for an address the client does not own yet before
it could deliver an offer. Addressing the reply to the client's hardware address —
what every DHCP server does — removes that failure mode entirely, so the server
lives in `source/winspire/bridge_net.c` and is unit tested by the host build
(`make selftest`). It handles DISCOVER/OFFER, REQUEST/ACK/NAK, DECLINE, RELEASE
and INFORM, quarantines declined addresses for 60 s, and builds validated
IP/UDP checksums over a full Ethernet+IPv4+UDP+BOOTP reply.

`main.c` keeps only the parts that need the ESP32: handing the frame to lwIP,
emitting a reply back over the link (`cxlink_send_net_rx()` is the single
guest-facing exit), NAPT, and the Wi-Fi uplink.

### Uplink configuration and provisioning

| Symbol | Where | Meaning |
|---|---|---|
| `CONFIG_LWIP_IP_FORWARD` | `sdkconfig.defaults`, `sdkconfig` | lwIP forwards between interfaces. `main.c` has an `#error` if it is off |
| `CONFIG_LWIP_IPV4_NAPT` | `sdkconfig.defaults`, `sdkconfig` | Source NAT. Same `#error` guard |

There is no SSID or password in the build configuration on purpose. They arrive
from the calculator in the `NET_CONFIG` frame (`CxlinkNetConfig.ssid` /
`.password`), because the calculator is this bridge's only console and a password
in build configuration is a password in source control. On the calculator they
come from a `[network]` section in `winspire.ini`; the whole path is described in
[NETWORK_ARCHITECTURE.md](../../NETWORK_ARCHITECTURE.md) §6.3.

What this firmware does with them:

- **Validates before applying**, with the same `cxlink_wifi_check()` the
  calculator runs, so a pair the calculator accepted is never one the bridge has
  to refuse. SSID 1-31 characters, password empty (open network) or 8-63; the
  limits sit one byte below ESP-IDF's 32/64-byte fields because those have to
  stay NUL-terminated.
- **Applies only a change.** Both fields of the pair in use are compared, so the
  calculator's retransmission (every 2 s until confirmed) is a no-op, while a
  password change with an unchanged SSID is applied.
- **Stores nothing in flash** (`WIFI_STORAGE_RAM`). After a power cycle the
  bridge reports "no credentials", and the calculator sends them again by itself.
- **Says what the radio did with them**, in every `NET_CONFIG` (every 2 s):
  `CXLINK_NET_FLAG_CREDENTIALS` means "held", `CXLINK_NET_FLAG_UPLINK_UP` means
  "associated and addressed", and `CxlinkNetConfig.state` is one of idle /
  associating / connected / rejected. A rejection (`WIFI_REASON_AUTH_FAIL`, the
  handshake timeouts, MIC failure, association failure) is reported as rejected so
  the calculator can say "the access point refused the credentials" rather than
  leaving a typo indistinguishable from an access point that is out of range.
- **Never logs the password.** The only trace of a successful apply is the SSID,
  and the pair lives in RAM for as long as the association does: no NVS entry, no
  config file, no flash write.

> **None of this has run against a real radio.** The credential exchange is
> verified on the workstation (`make selftest`, and the `wifi provision` line from
> the host frontend), but `esp_wifi_connect()` has never been called with a real
> pair, so neither a successful association nor a rejected one has been observed.

### What has not been run

The counters make the difference between "built" and "worked" visible:
`NET_STATUS` reports `tx_frames` (guest -> uplink), `rx_frames` (uplink -> guest),
`tx_dropped`, and `link_errors` (duplicate `NET_TX` retransmissions). They are
sent every 2 s and are zero on an idle link.

Zero is also what to expect from a first hardware session for a second reason:
none of DHCP, NAPT, forwarding or the Wi-Fi association has ever executed. The
DHCP server is the exception — it runs in `make selftest` — and that test is what
caught three real defects while it was written; credential validation and the
provisioning exchange are in the same suite. Everything else here is a careful
read of lwIP 2.2.0 and the ESP-IDF sources, not a measurement.

One bug in this file was found by reading the *calculator's* side of the link
rather than by any test: this firmware used to report the Wi-Fi uplink state in
`CXLINK_NET_FLAG_LINK_UP`, which the calculator took to mean "the bridge is
alive". A bridge with no credentials therefore looked absent — the audio path
went silent and the guest's DHCP could not even start. Liveness now comes from
frames arriving, and the uplink has its own flag; see NETWORK_ARCHITECTURE.md §6.4.

## Bandwidth

115 200 8N1 is 11 520 B/s. Framing costs 10 bytes per frame. At the default
8 kHz mono 8-bit audio (~8 620 B/s of the link) roughly **2 900 B/s** remain for
Ethernet. That is enough for ping, DNS and small requests; it is not enough for
bulk transfer. Lowering or muting the audio rate frees the whole link for
networking — `CXLINK_AUDIO_FLAG_MUTE` exists for exactly that.

Tuning loop: the calculator reports `tx_frames`, `rx_frames`, `tx_dropped` and
`link_errors` via `cxlink_get_status()` (the firmware sends the same four in
`NET_STATUS` every 2 s), and audio underruns arrive in `AUDIO_STATUS`. Watch
those before changing anything. `link_errors` on the network side is the
duplicate `NET_TX` count, so a climbing value means the link is losing frames and
retrying, not that the guest is misbehaving.

## Debugging the link

The most useful signal is the decoder's error counters, which the calculator
keeps (`frames_dropped_crc`, `frames_dropped_format`):

- **CRC failures climbing** → a marginal physical link. Shorten the leads and
  check the ground before suspecting the protocol.
- **Format failures climbing** → a desynchronised stream, usually a baud-rate or
  framing mismatch. Confirm 115200 8N1 on both ends.
- **Nothing at all** → the calculator side has no transport unless it was built
  with `-DWINSPIRE_CXLINK_UART`, and that flag depends on the unverified UART
  register base. Check HARDWARE.md §2.1 first.

A logic analyser on the calculator's dock Tx pin (pin 4) settles in seconds what
otherwise takes an afternoon of guessing.
