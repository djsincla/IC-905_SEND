# Release Notes

All notable changes to **IC-905 SEND**. ([splash page](https://djsincla.github.io/IC-905_SEND/))

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
