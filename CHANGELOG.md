# Release Notes

All notable changes to **IC-905 SEND**. ([splash page](https://djsincla.github.io/IC-905_SEND/))

## v1.3 — 2026-05-26
- **Running lean:** trimmed the Pi to an appliance-minimal service set so nothing competes with the sequencer (which already has a dedicated core via `CPUAffinity=3`). Disabled as unused — the leftover **PM2/Node.js** daemon (from the retired Node-RED host), **Bluetooth** (`bluetooth` + `hciuart`), **ModemManager**, and **triggerhappy**. Kept SSH, WiFi (NetworkManager/wpa_supplicant), `avahi` (resolves `pi5.local`), time sync, mosquitto, and the relay service. Idle load ≈ 0.0, ~330 MB RAM. See **Running lean** in the README.

## v1.2 — 2026-05-26
- **Glitch-free relay init:** clear the PCA9538A output registers *before* switching the pins to outputs. Previously, because the expander's output register powers up at `0xFF`, enabling the outputs first could briefly drive the relay pins HIGH and momentarily close every relay at startup. Relays now go straight from high-Z to OPEN with no transient. (No deliberate relay cycling happens at startup either way — the service waits for a decoded TX before touching a relay.)

## v1.1 — 2026-05-26
- **CPU isolation safeguard:** pin the relay sequencer to CPU core 3 (`CPUAffinity=3` in the systemd unit). Combined with `Nice=-10`, the latency-critical relay timing keeps a core to itself while the MQTT broker, any monitoring, and the OS run on cores 0–2 — so they can never jitter the sequencing.
- Service now reports its version at startup.

## v1.0 — 2026-05-26
- **Band-specific TX detection over pure Ethernet** — reads the controller's explicit transmit command (byte 38 of the `0x44` status frame); works on every band, **2m through 10 GHz**.
- **Band & frequency decode** from offset 184 of the same frame; IF-aware classification (true frequency on 2m, an IF value on the higher bands) with midpoint-centered thresholds verified on-air for all six bands.
- **Timed relay sequencing** — per-relay millisecond delays close in order on TX and open mirrored on RX; multi-band rules and per-band delays supported.
- **Authenticated MQTT** monitoring + control via a local mosquitto broker, on its own thread so it never blocks the relay path.
- Native C systemd service driving two PCA9538A I²C relay boards on a Raspberry Pi.
- Documentation, sample configs, and a GitHub Pages splash page (burnt-orange theme).
