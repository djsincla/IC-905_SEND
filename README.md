# IC-905 SEND — Packet Capture, Relay Sequencer & MQTT

Automatic antenna/amplifier band switching for the **Icom IC-905** microwave/VHF/UHF transceiver.

A Raspberry Pi sits on a network tap between the IC-905 *controller* and its *RF deck*, sniffs the Ethernet traffic between them, decodes the selected band and TX/RX state, and drives I²C relays to route the correct antenna/amp path — only while transmitting.

> **Runtime:** a native C systemd service, `ic905-relay` — libpcap for capture, libgpiod + I²C for the relays, all on the Raspberry Pi.

---

## Architecture

```
IC-905 Controller ──┐
                    ├── network tap ──► Pi eth0 (libpcap capture)
IC-905 RF Deck ─────┘                        │
                                             ▼
                              decode band + TX state from TCP payload
                                             │
                                             ▼
                          I²C (/dev/i2c-1) → 2× PCA9538A relay boards
```

The C program is designed for **sub-10ms latency**: libpcap in immediate mode (no kernel buffering), 1 ms poll timeout, edge-triggered relay writes (only changed boards are written).

### Startup sequence (`main()`)
1. **GPIO reset** both PCA9538A boards — pulse reset pins LOW 100 ms, then HIGH (`gpiochip0`, GPIO5 + GPIO12), then release.
2. **I²C init** — open `/dev/i2c-1`, set both boards' config register to `0xF8` (P0–P2 outputs, P3–P7 inputs), open all relays.
3. **Packet capture** — `pcap` on `eth0` with the BPF filter below.
4. **Capture loop** — decode each matching packet, actuate relays on change.
5. **Shutdown** (SIGINT/SIGTERM) — open all relays, clean up.

---

## Hardware

| Item | Detail |
|---|---|
| Relay boards | 2× MikroElektronika **Relay 5 Click**, PCA9538A I²C GPIO expander |
| I²C bus | `/dev/i2c-1` (must be enabled via `raspi-config` → Interface → I2C) |
| Board 1 address | `0x70` — bands 144 / 430 / 1200 MHz |
| Board 2 address | `0x73` — bands 2400 / 5600 MHz / 10 GHz |
| Reset pins | GPIO5 → Board 1, GPIO12 → Board 2 (`gpiochip0`; on Pi 5 this is the 40-pin header — `gpiochip4` is a symlink to it) |
| Capture | network tap on `eth0` between IC-905 controller and RF deck |

### Relay → board → I²C → pin

Both Click boards share the I²C bus (`/dev/i2c-1`) and are distinguished only by address (`0x70` / `0x73`, set by each board's address pins). On each board the three relays hang off PCA9538A pins **P0–P2**, switched via the output register (`0x01`); P3–P7 are inputs (config register `0x03 = 0xF8`). The software relay numbers `1`–`6` map to boards in order — 1–3 = Board 1, 4–6 = Board 2:

| Relay (config) | Click board | I²C addr | PCA9538A pin | Output-reg bit |
|---|---|---|---|---|
| `1` | Board 1 | `0x70` | P2 | `0x04` |
| `2` | Board 1 | `0x70` | P1 | `0x02` |
| `3` | Board 1 | `0x70` | P0 | `0x01` |
| `4` | Board 2 | `0x73` | P2 | `0x04` |
| `5` | Board 2 | `0x73` | P1 | `0x02` |
| `6` | Board 2 | `0x73` | P0 | `0x01` |

To switch a relay the service recomposes that board's whole output byte from all its currently-closed relays and writes register `0x01`, so relays on the same board update in a single I²C write. Reset lines are separate GPIOs: **GPIO5 → Board 1, GPIO12 → Board 2**.

---

## Packet decode

- **BPF filter:** `dst port 50004` — the whole controller→deck control stream.
- **Status/command frame:** band, frequency, and TX state all live in one frame family, identified by **signature, not size** (the frame size differs per band): `payload[0] == 0x01 && payload[10] == 0x44`. These frames appear at key edges and band/frequency changes.
- **Capture must be promiscuous.** The tap delivers frames addressed to the radio's MACs (`02:90:c7:…` / `00:90:c7:…`), not the Pi's, so `pcap_set_promisc()` must be **1**. Verify: `cat /sys/class/net/eth0/flags` → bit `0x100` set (note: `ip link` may not print `PROMISC` even when it is).
- **TX state:** **byte 38** of the status frame — `1` = transmitting, `0` = receiving. This is the controller's explicit transmit command to the RF deck, sent at every key edge **for every band** (the lower bands flag TX nowhere else). Verified on-air, 2m through 10 GHz.
- **Band / frequency:** 4-byte little-endian `uint32` at offset **184 from the start** of the payload (present only in the larger frames of the family — the small key-edge frames carry TX but no frequency, so the band is latched from the last frequency frame). The field is the **true frequency only for 2m**; every higher band reports an **IF value** the radio up-converts from (a uint32 in Hz can't even hold 5.6/10 GHz). Measured IF per band, with the midpoint-centered classification thresholds:

  | Band | Real freq | Reported IF | Threshold |
  |---|---|---|---|
  | 2m | 144 MHz | 144.1 MHz (true) | < 189 MHz |
  | 70cm | 430 MHz | 233.1 MHz | < 320 MHz |
  | 23cm | 1296 MHz | 407.0 MHz | < 487 MHz |
  | 13cm | 2400 MHz | 566.1 MHz | < 820 MHz |
  | 6cm | 5600 MHz | 1073.0 MHz | < 1415 MHz |
  | 3cm | 10 GHz | 1757.3 MHz | < 3000 MHz |
  | — | — | else | Unknown |

---

## Relay sequencing — `/etc/ic905-relay.conf`

The band→relay map is **user-editable** and supports **timed sequencing**: multiple relays per band, each with a millisecond delay. Relays are numbered **1–6** (1–3 = board 1 `0x70`, 4–6 = board 2 `0x73`). See [`CONFIG-EXAMPLES.md`](CONFIG-EXAMPLES.md) for ready-to-use sample configs.

```
# relay, band, delay_ms
# band: 2m 70cm 23cm 13cm 6cm 3cm  (or 144 430 1200 2400 5600 10g) or  all
1, 23cm, 0       # closes immediately on 23cm TX
2, 23cm, 10      # +10 ms
4, 23cm, 20      # +20 ms
3, all,  0       # closes on ANY band TX (e.g. amp PTT)
5, 2m/70cm, 0    # ONE relay shared by two bands (2m AND 70cm)
6, 23cm/2m, 0/15 # mixed bands with per-band delays: 0 ms on 23cm, 15 ms on 2m
```

- **On TX:** matching relays **close in increasing delay order**.
- **On RX:** they **open in mirrored reverse order** — last-closed opens first, same gaps (off-offset = `maxDelay − thisDelay`).
- **Band change while keyed:** ramp the old band down (mirrored), then the new band up.
- `all` matches any band; a specific-band rule wins over `all` for the same relay.
- **Multiple bands per relay:** list them with `/` in the band field — e.g. `1, 23cm/2m, 0`. The delay is shared across the listed bands, or give one per band with `/` (e.g. `1, 23cm/2m, 0/15` → 23cm @ 0 ms, 2m @ 15 ms).
- A relay may appear on several lines (one per band). Boards with no rules are left untouched.
- Timing is driven by a `poll()`-based scheduler (≈1 ms resolution, single-threaded). Edit, then `sudo systemctl restart ic905-relay`; parsed rules are logged at startup.

---

## Files

| File | Purpose |
|---|---|
| `ic905_relay.c` | The controller (libpcap + libgpiod + I²C) |
| `Makefile` | Build / install / uninstall |
| `ic905-relay.service` | systemd unit (runs as root, `Restart=on-failure`, `Nice=-10`) |
| `ic905-relay.conf` | band→relay sequencing rules + MQTT settings (installed to `/etc/`, not clobbered on reinstall) |
| `CONFIG-EXAMPLES.md` | ready-to-use sample relay/band/sequencing configs |
| `docs/index.html` | project splash page (GitHub Pages) |

---

## Build & deploy

### Target environment
- Raspberry Pi 5, Raspberry Pi OS / Debian 12 (Bookworm)
- Host `pi5.local`, user `dwayne`, key-based SSH (`~/.ssh/id_ed25519`), passwordless sudo
- Build deps: `build-essential`, `libpcap-dev`, `libgpiod-dev` (libgpiod 1.6 = v1 API), `libmosquitto-dev`. Local broker: `mosquitto` + `mosquitto-clients`.
- **`eth0` is the tap port** — set it to link-only in NetworkManager so it doesn't endlessly retry DHCP (`no lease` churn): `sudo nmcli connection modify "Wired connection 1" ipv4.method disabled ipv6.method disabled`. Management/SSH is over `wlan0`.

```bash
sudo apt install -y build-essential libpcap-dev libgpiod-dev \
    libmosquitto-dev mosquitto mosquitto-clients   # one-time
```

### Deploy from the Mac
```bash
# 1. Push source to the Pi
rsync -av --exclude='*.o' --exclude='ic905-relay' \
  "/Users/dwayne/Developer/IC-905 SEND/" dwayne@pi5.local:~/ic905/

# 2. Build, install, and load the new binary
ssh dwayne@pi5.local 'cd ~/ic905 && make && sudo make install && sudo systemctl restart ic905-relay'
```

> ⚠️ `make install` only copies the binary + unit and runs `daemon-reload` — it does **not** restart a running service. You must `systemctl restart` to actually load a new binary.

### Makefile targets
| Target | Action |
|---|---|
| `make` | Build `ic905-relay` |
| `sudo make install` | Install to `/usr/local/bin`, install unit, `daemon-reload` |
| `sudo make uninstall` | Stop/disable service, remove binary + unit |
| `make clean` | Remove build artifacts |

---

## Service management

```bash
sudo systemctl enable --now ic905-relay     # start + enable on boot
sudo systemctl restart ic905-relay          # reload after a new build
sudo systemctl status ic905-relay
journalctl -u ic905-relay -f                # live logs
```

---

## Testing / verification

```bash
# Service is up and capturing?
systemctl is-active ic905-relay
journalctl -u ic905-relay -n 20 --no-pager
#   expect: "PCA9538A reset pulse complete" → "boards initialized" → "Entering capture loop"

# Both relay boards present on the I²C bus?
sudo apt install -y i2c-tools          # if needed
i2cdetect -y 1                         # expect devices at 0x70 and 0x73

# Watch band/TX decode + relay switching during a real transmit:
journalctl -u ic905-relay -f
#   key up on each band — expect "Band: X -> Y" and "TX: ON/OFF (band)" lines

# Reboot test (proves it comes up clean on its own):
sudo reboot
#   after it returns: systemctl is-active ic905-relay  → active
```

---

## MQTT monitoring & control

Optional, over WiFi. Configured by `mqtt_*` directives in `/etc/ic905-relay.conf`; **disabled unless `mqtt_enable = 1`**. A missing or down broker never affects relay timing — network I/O runs on a separate thread, inbound commands flow through a queue drained on the relay thread, and publishes are non-blocking (after the I²C write).

**Local broker (authenticated):** `mosquitto` on the Pi, port 1883, **password auth** (`allow_anonymous false`, `password_file /etc/mosquitto/ic905.passwd`, user `ic905`). Dashboards connect to `192.168.4.50:1883` with the credentials.

```
mqtt_enable = 1
mqtt_broker = 127.0.0.1
mqtt_port   = 1883
mqtt_prefix = ic905
mqtt_user   = ic905
mqtt_pass   = <password>
```

**Topics (retained):**
| Topic | Payload |
|---|---|
| `ic905/status` | `online` / `offline` (offline via last-will) |
| `ic905/band` | e.g. `1200 MHz` |
| `ic905/tx` | `ON` / `OFF` |
| `ic905/relay/<1-6>` | `close` / `open` |
| `ic905/relay/<1-6>/mode` | `auto` / `manual` |
| `ic905/state` | JSON `{"band","tx","relays":[…],"modes":[…]}` |

**Commands** (service subscribes `ic905/cmd/#`):
| Topic | Payload | Effect |
|---|---|---|
| `ic905/cmd/relay/<N>` | `close` / `open` | lock relay N manually (sequencer skips it) |
| `ic905/cmd/relay/<N>` | `auto` | release N back to the sequencer |
| `ic905/cmd/mode` | `manual` | freeze all relays at current state |
| `ic905/cmd/mode` | `auto` | release all relays to the sequencer |

```bash
# watch everything
mosquitto_sub -h 192.168.4.50 -u ic905 -P <pw> -t 'ic905/#' -v
# manually close relay 5, then release it
mosquitto_pub -h 192.168.4.50 -u ic905 -P <pw> -t ic905/cmd/relay/5 -m close
mosquitto_pub -h 192.168.4.50 -u ic905 -P <pw> -t ic905/cmd/relay/5 -m auto
```

> ⚠️ Manual relay commands are honored even during TX (a warning is logged). Switching relays under RF can hot-switch — operator's responsibility.

---

## Troubleshooting

**Crash-loop with `Failed to request GPIO outputs: Device or resource busy`**
Something else holds GPIO5/GPIO12 (the PCA9538A reset pins). Find the holder:
```bash
sudo gpioinfo gpiochip0 | grep -E 'line +(5|12):'   # shows consumer name
```
Stop whatever claims those lines, then `sudo systemctl restart ic905-relay`.

**No decode even though traffic is flowing**
- Check promiscuous mode is on: `cat /sys/class/net/eth0/flags` → must have bit `0x100`. The kernel also logs `eth0: entered promiscuous mode` at service start (`journalctl -k | grep promisc`).
- The status frame is **event-driven** (sent on band change / TX edges), not continuous — you must transmit or change band to see it. Confirm it's on the wire: `sudo tcpdump -nv -i eth0 "dst port 50004"` while keying up, and look for a frame whose payload begins `01 …` with `0x44` at byte 10.
- Right after a service restart the band is **Unknown** until the first frequency frame arrives, so the very first key-down may not sequence until you tune or complete one key cycle.

**No packets at all**
- Confirm the tap feeds `eth0` and the IC-905 traffic uses TCP dst port 50004.
- Watch counts (promiscuous): `sudo tcpdump -i eth0 -c 5 'dst port 50004'`.

**I²C errors in the log**
- `i2cdetect -y 1` should show `0x70` and `0x73`. Check wiring/power and that I²C is enabled.

