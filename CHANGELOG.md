# Release Notes

All notable changes to **IC-905 SEND**. ([splash page](https://djsincla.github.io/IC-905_SEND/))

## v1.11 — 2026-05-27
- **Relays sequence the actual TX band in split.** If a frame asserts TX (`byte 38 = 1`) on a band different from the one displayed/received — i.e. split with the transmit VFO on another band (RX 23cm / TX 2m) — the relays now follow the **transmitting** band. The dual-watch sub-VFO (non-transmitting, different band) is still ignored during TX. This closes a corner case where a mis-set split could otherwise have sequenced the wrong (displayed) band's relays.

## v1.10 — 2026-05-27
- **Split + sub-VFO in MQTT:** new topics **`ic905/split`** (`on`/`off`, decoded from byte 236 bit 7 on idle frames) and **`ic905/band_b`** / **`ic905/freq_b`** (the dual-watch sub-VFO from byte 196 — e.g. main 23cm `1296.117.007` → sub `2m 144.375.000`). All three also appear in `ic905/state` and the `Split:` log line.
- Power is now read **only on TX frames**, so split-enabled-while-idle no longer makes `ic905/power` read a false 50% (byte 236 is the forward-power meter during TX, the split bit when idle).

## v1.9 — 2026-05-27
- **Code-audit hardening / cleanup** (no behaviour change): removed dead `g_last_activity` and the stale "stream-silence" comments; log dropped MQTT commands when the queue is full; defensive `snprintf` bounds in the `state` JSON; documented the little-endian and I²C log-and-continue assumptions. A full senior-C audit found **no critical issues** — concurrency model, relay sequencer/scheduler, and packet bounds all verified sound.
- **Splash version fixed:** the page footer fallback now matches the release and the changelog fetch is cache-busted, so the shown version no longer lags.
- Radio re-labelled **VHF/UHF/SHF transceiver** (was "microwave").

## v1.8 — 2026-05-27
- **Power on the status topic:** `ic905/status` now reads `online 50%` (liveness + current power level), `online` when power is unknown, and `offline` via the last-will on disconnect.

## v1.7 — 2026-05-27
- **TX power fixed for all bands:** the power byte is at a fixed offset (236) and only appears in the radio's full (~240-byte) status frame; the abbreviated frames were clobbering it to 0. Now read byte 236 only when present, and never overwrite a known value from a short frame (power also resets on band change). Verified 23cm 25%, 3cm 10%.
- **All six frequency offsets confirmed on-air** and locked into the defaults: 2m=0, 70cm=199, 23cm=889, 13cm=1738, 6cm=4687, 3cm=8611 MHz (corrected the earlier 13cm/6cm estimates). Still overridable via `freq_offset_<band>`.
- **Spurious sub-VFO ignored:** while transmitting, frequency frames for a *different* band (the dual-watch sub-VFO — e.g. 2m parked at 144.375 while you're on 3cm) are ignored, so they can't flip the band or drop TX mid-transmit.
- **Power on the TX topic:** `ic905/tx` now reads e.g. `ON 25%` when keyed (power level included), `OFF` otherwise.
- Note: power needs the full status frame, which not every band sends on every key — a band that only emits abbreviated frames will report power as unknown.

## v1.6 — 2026-05-27
- **Frequency shown as `MHz.kHz.Hz`** (e.g. `1296.117.007`) in the logs and in MQTT — `ic905/freq` and the `freq` field of `ic905/state` (now a string).
- **70cm calibration confirmed:** offset 199 MHz (IF 233.065 + 199 = 432.065). Confirmed offsets are now 2m=0, 70cm=199, 23cm=889; 13cm/6cm/3cm still estimates.
- **Known issue:** TX power decodes correctly only on 23cm; on 2m/70cm `ic905/power` reads 0 (the power byte sits at a different offset in those smaller frames) — fix pending a capture at a distinctive power level.

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
