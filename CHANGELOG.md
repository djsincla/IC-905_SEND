# Release Notes

All notable changes to **IC-905 SEND**. ([splash page](https://djsincla.github.io/IC-905_SEND/))

## v1.5 — 2026-05-26
- **Actual on-air frequency:** the published/logged frequency is now the true RF, computed as `IF + per-band LO offset`. The IF tracks the dial 1:1, so it's exact to the Hz — verified on 23cm (IF 407.117 + 889 MHz = 1296.117 MHz, and a 7 Hz dial nudge shows as 7 Hz). The freq field is now 64-bit (6cm/3cm RF exceeds 32 bits).
- **Per-band calibration:** offsets `2m = 0` and `23cm = 889 MHz` are confirmed on-air; `70cm`/`13cm`/`6cm`/`3cm` are estimates pending confirmation. Override any in the config: `freq_offset_<band> = <MHz>` (e.g. `freq_offset_23cm = 889`).

## v1.4 — 2026-05-26
- **TX power in MQTT:** new `ic905/power` topic (and a `power` field in `ic905/state`) — the radio's TX-power setting as a percentage, decoded from the freq frame (verified on 23cm: `0x40` = 25%).
- **Band as wavelength:** `ic905/band`, the state JSON, and the `Band:` / `TX:` log lines now use ham wavelength names (`2m`, `70cm`, `23cm`, …) and include the frequency in MHz.
- **Note on frequency:** the published/​logged frequency is the radio's reported value — the true on-air RF on **2m**, and the **IF** on the higher bands. The actual RF for bands above 2m is *not* transmitted in the packet (verified by scanning the frame), so true on-air RF there would require a per-band calibration offset.

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
