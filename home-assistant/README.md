# Home Assistant integration

`ic905-relay` v1.19+ publishes [Home Assistant MQTT discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) messages on connect, so HA auto-creates a complete **IC-905 SEND** device with ~30 entities — no HA-side config beyond pointing HA at the broker. Adding new fields later (e.g. AGC) is one publish call in the C service; HA picks it up automatically on the next service restart.

---

## What you get in HA

A single device **"IC-905 SEND"** with these entities:

| Type | Entity | Purpose |
|---|---|---|
| sensor | `sensor.ic905_band` | operating (TX) VFO band, e.g. `23cm` |
| sensor | `sensor.ic905_freq` | operating-VFO RF as `MHz.kHz.Hz` |
| sensor | `sensor.ic905_band_b` | the other VFO band |
| sensor | `sensor.ic905_freq_b` | the other VFO freq |
| sensor | `sensor.ic905_power` | TX power %, with history graphable |
| sensor | `sensor.ic905_status` | service liveness |
| binary_sensor | `binary_sensor.ic905_tx` | on while transmitting |
| binary_sensor | `binary_sensor.ic905_split` | split on/off |
| binary_sensor | `binary_sensor.ic905_preamp` | preamp on/off |
| binary_sensor | `binary_sensor.ic905_atten` | attenuator on/off |
| switch ×6 | `switch.ic905_relay_<1-6>` | per-relay close (on) / open (off); clicking forces manual mode |
| button ×6 | `button.ic905_relay_<1-6>_auto` | release that relay back to the sequencer |
| sensor ×6 | `sensor.ic905_relay_<1-6>_mode` | per-relay `auto` / `manual` text |
| button | `button.ic905_mode_auto` | release **all** relays to the sequencer |
| button | `button.ic905_mode_manual` | freeze **all** relays at current state |

All under one device card, with mdi icons and IC-905 SEND identifying info (model, manufacturer, software version, configuration URL = the GitHub repo).

---

## Setup — three steps

### 1. Stand up Home Assistant on a separate box (any LAN-connected machine that's already always-on)

**Docker (recommended, cross-platform):**

```bash
mkdir -p ~/ha-config
docker run -d --name homeassistant --restart unless-stopped \
  --network=host \
  -e TZ=America/Los_Angeles \
  -v ~/ha-config:/config \
  ghcr.io/home-assistant/home-assistant:stable
```

Then open `http://<ha-host>:8123` and complete the onboarding (admin account, location, etc.).

**Alternatives:** [Home Assistant OS](https://www.home-assistant.io/installation/) (a full image) if the box can be dedicated; `pip install` if you really insist.

### 2. Connect HA to the pi5 mosquitto broker

In HA: **Settings → Devices & Services → Add Integration → MQTT**

- **Broker:** `pi5.local` (or the Pi's IP — see DHCP-reservation note below)
- **Port:** `1883`
- **Username:** `ic905`
- **Password:** the value of `mqtt_pass` in `/etc/ic905-relay.conf` on the Pi
- **Discovery:** **enabled** (default), prefix `homeassistant`

Save. Within seconds the IC-905 SEND device appears under **Devices**.

> If `pi5.local` mDNS is flaky on the HA host, use the Pi's IP directly. (After WiFi flaps the Pi has been swapping between `192.168.4.50` and `.51`; a DHCP reservation in your router avoids this once and for all.)

### 3. Import the sample dashboard (optional)

The repo includes [`lovelace-ic905.yaml`](./lovelace-ic905.yaml) as a starting point. In HA:

**Settings → Dashboards → + ADD DASHBOARD → Start with raw config** → paste the file's contents → save.

You'll get a single "Radio" view with status, both VFOs, the 6 relays, and a 24-hour history graph of TX activity + power.

---

## Mobile

Install **Home Assistant Companion App** ([iOS](https://apps.apple.com/us/app/home-assistant/id1099568401) / [Android](https://play.google.com/store/apps/details?id=io.homeassistant.companion.android)), log in to the same HA instance, and the dashboard is on your phone — with native widgets, lock-screen panels, and push notifications you can wire to events like `binary_sensor.ic905_tx` going `on` while you're away from the shack.

---

## Behaviour notes

- **Switch click → relay goes manual.** Clicking `switch.ic905_relay_4` to "on" sends `close` to `ic905/cmd/relay/4`; the service closes the relay and marks it manual (sequencer skips it). Click again to open. Use the matching `button.ic905_relay_4_auto` to release it back to the sequencer.
- **Discovery is idempotent.** The service re-publishes the discovery messages on every MQTT (re)connect. Retained, so HA picks them up regardless of who connects first.
- **Adding new MQTT fields later** (e.g. AGC) is a one-line addition in `ic905_relay.c`'s `mqtt_publish_ha_discovery()`. HA auto-creates the entity next time the service publishes.
- **Removing entities:** publish an empty payload to the discovery topic with retain=true to clear an old one (HA will then drop the entity).

---

## Opting out

If you'd rather not have discovery messages published (e.g. another HA instance on the same broker that shouldn't see this device), set in `/etc/ic905-relay.conf`:

```
mqtt_ha_discovery = 0
```

…and restart. All other MQTT topics (`ic905/*`) keep working — you simply lose the auto-discovery convenience.

---

## Remote access (out of scope here)

LAN-only is the default. When you want it from anywhere:

- **Easiest:** [Tailscale](https://tailscale.com/) — free, install on the HA host + your phone, no port-forwarding. The Companion app then works from anywhere.
- **Official:** [Nabu Casa Cloud](https://www.nabucasa.com/) — $6.50/mo, zero config, full remote dashboard.
