# ic905-relay — sample configurations

`/etc/ic905-relay.conf` maps **bands → relays** with optional **sequencing delays**.

## Syntax

```
<relay>, <band>, <delay_ms>
```

- **`<relay>`** — `1`–`6`. Relays 1–3 are on board 1 (I²C `0x70`), 4–6 on board 2 (`0x73`).
- **`<band>`** — `2m 70cm 23cm 13cm 6cm 3cm` (or `144 430 1200 2400 5600 10g`), or `all` for every band. Several bands can share one rule with `/`: `23cm/2m`.
- **`<delay_ms>`** — milliseconds after the TX edge before this relay closes. Per-band delays can also be `/`-listed: `23cm/2m, 0/15`.

**Sequencing:** on **TX**, the relays matching the band close in increasing-delay order. On **RX** they open in mirrored reverse order — the last-closed opens first, with the same gaps (`off-offset = maxDelay − thisDelay`). A band change while keyed ramps the old band down, then the new band up.

Lines starting with `#` are comments. Edit, then `sudo systemctl restart ic905-relay`.

---

## 1. Two-band sequences, one per board (current)

23cm sequences board 1; 13cm sequences board 2.

```
1, 23cm, 0
2, 23cm, 10
3, 23cm, 10
4, 13cm, 0
5, 13cm, 10
6, 13cm, 20
```

## 2. One relay per band (1:1)

Each band lights exactly one relay — handy for testing or simple band indication.

```
1, 2m,   0
2, 70cm, 0
3, 23cm, 0
4, 13cm, 0
5, 6cm,  0
6, 3cm,  0
```

## 3. Amp PTT after the antenna relay (sequenced TX)

Switch the antenna/coax relay first, then key the amplifier 25 ms later so the
relay is settled before RF — protects the contacts. On RX the amp un-keys first,
then the antenna relay releases.

```
1, 23cm, 0     # coax/antenna relay closes immediately
2, 23cm, 25    # amp PTT keys 25 ms later
```

## 4. All-band amp PTT + per-band antenna select

Relay 3 keys the amp on **any** band; relays 1/2 select the antenna per band.

```
1, 2m,   0     # 2m antenna
2, 70cm, 0     # 70cm antenna
3, all,  20    # amp PTT on every band, 20 ms after the antenna relay
```

## 5. One relay shared across two bands

A single relay serves both 2m and 70cm (e.g. a shared VHF/UHF antenna).

```
5, 2m/70cm, 0
```

## 6. Shared relay with per-band delays

Same relay, different delay per band (delays `/`-listed to match the bands).

```
1, 23cm/2m, 0/15   # 0 ms on 23cm, 15 ms on 2m
```

---

## Band → IF reference

The radio reports the true frequency only on 2m; every higher band reports an IF
value the IC-905 up-converts from. Measured IF per band (what classification keys off):

| Band | Real freq | Reported IF |
|---|---|---|
| 2m   | 144 MHz  | 144.1 MHz (true) |
| 70cm | 430 MHz  | 233.1 MHz |
| 23cm | 1296 MHz | 407.0 MHz |
| 13cm | 2400 MHz | 566.1 MHz |
| 6cm  | 5600 MHz | 1073.0 MHz |
| 3cm  | 10 GHz   | 1757.3 MHz |

The relay logic and MQTT key off the decoded **band**, not the raw IF value.
