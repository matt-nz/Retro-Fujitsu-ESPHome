# Fujitsu Heat Pump Protocol Notes

## Capture Format and Candidate Frame

The ESPHome log records UART reads in chunks of up to 8 bytes; those reads are not
guaranteed to start at a protocol frame boundary. The logged bus bytes decode to the
candidate frames below by XORing each byte with `0xFF`. The ESPHome UART is configured
for 500 baud, 8E1.

The full-round fan-speed capture repeatedly contains this candidate 8-byte frame:

```
Decoded:      7F 22 03 <byte3> 02 5F 7E 7F
Captured bus: 80 DD FC <~byte3> FD A0 81 80
```

In that capture, byte 3 is the only byte that changes within this frame as the selected
fan speed changes. This establishes a repeatable correlation for this capture, not the
meaning of every byte or the frame's role on the bus. In particular, `0x22` is the
destination value produced by the current decoder's address mask; it has not been
confirmed as the wired controller's address.

Earlier address, field, checksum, and command-sequence interpretations in this note are
hypotheses, not a validated protocol specification. The checksum equations previously
listed here do not fit this candidate frame: decoded byte 5 is `0x5F`, while
`0x7F XOR 0x22` is `0x5D`; decoded byte 6 remains `0x7E` as byte 3 changes. Do not rely
on those equations or transmit based on this note.

## Example Frames

### Fan Low (operator-labeled capture)
```
Decoded:      7F 22 03 09 02 5F 7E 7F
Captured bus: 80 DD FC F6 FD A0 81 80
```

### 17°C Setpoint (operator-labeled capture)
```
Decoded:      7F 12 03 32 02 5F 7E 7F
Captured bus: 80 ED FC CD FD A0 81 80
```

### Other observed frame
```
Decoded:      7F 28 13 48 80 7F 28 17
Captured bus: 80 D7 EC B7 7F 80 D7 E8
```

## Live Capture Findings

Passive captures on this unit confirm that the captured bytes decode by XORing each byte
with `0xFF`. The following setpoint changes were made in HEAT mode with the unit powered on:

| Controller setpoint | Decoded byte 3 | Captured bus byte 3 |
|---|---:|---:|
| 18°C | `0x52` | `0xAD` |
| 19°C | `0x72` | `0x8D` |
| 20°C | `0x92` | `0x6D` |
| 21°C | `0xB2` | `0x4D` |
| 22°C | `0xD2` | `0x2D` |
| 23°C | `0xF2` | `0x0D` |
| 24°C | `0x12` | `0xED` |

The 18→19→18°C test was repeated while powered on and produced `0x52`→`0x72`→`0x52`.
The 20→22→18°C test produced `0x92`→`0xD2`→`0xB2`→`0x92`→`0x72`→`0x52`. A later
23→24→23°C test produced `0xF2`→`0x12`→`0xF2`, confirming the `+0x20` step wraps at
`0xFF`. These observations apply to the tested HEAT frames; they do not establish that
every frame type or operating mode uses the same byte layout.

In COOL mode, a separate 23→24→23°C test produced decoded byte 3 values
`0xBC`→`0xC4`→`0xBC` (captured bytes `0x43`→`0x3B`→`0x43`). This confirms the byte
tracks the setpoint in COOL mode too, but the observed one-degree change is `+0x08`,
not the `+0x20` seen in HEAT. A combined HEAT/Cool capture at 23°C showed `0xF2` in
HEAT and `0xBC` in COOL, confirming the mode dependence at the same setpoint.

The `hot-fan-cold-auto.log` mode cycle, with no reported setpoint or fan-speed changes,
produced these decoded byte 3 values:

| Selected mode | Decoded byte 3 | Captured bus byte 3 |
|---|---:|---:|
| HEAT | `0xF2` | `0x0D` |
| FAN | `0x09` | `0xF6` |
| COOL | `0xBC` | `0x43` |
| AUTO | `0x79` | `0x86` |
| HEAT again | `0xF2` | `0x0D` |

This confirms repeatable byte values correlated with the selected operating modes in
that cycle. However, `0x09` also appeared during an earlier fan-speed test labeled LOW,
so byte 3 should not yet be described as a standalone mode field.

A dedicated fan-speed capture was then made with the unit held in HEAT at 23°C, changing
the wired controller's fan setting LOW → AUTO → HIGH → MEDIUM → LOW. Four successive
candidate frames changed decoded byte 3 as follows:

| Selected fan setting | Decoded byte 3 | Captured bus byte 3 |
|---|---:|---:|
| AUTO | `0xF6` | `0x09` |
| HIGH | `0xE6` | `0x19` |
| MEDIUM | `0xEA` | `0x15` |
| LOW | `0xF2` | `0x0D` |

These values follow the reported setting order after the initial LOW state and return
to `0xF2` at the final LOW step. This is stronger evidence that byte 3 changes with fan
speed while mode and setpoint are held constant. The ESPHome log reported `Control
enabled: YES -- LIVE`, but the ESP32 TX wire is physically disconnected, so the ESP32
has no electrical path to transmit onto the heat-pump bus. This is a software setting,
not evidence that the ESP sent commands during this passive capture.

A second fan-speed cycle was captured in FAN mode, starting and ending at LOW:
LOW → AUTO → HIGH → MEDIUM → LOW. The four intervening candidate frames produced:

| Selected fan setting | Decoded byte 3 | Captured bus byte 3 |
|---|---:|---:|
| AUTO | `0x0B` | `0xF4` |
| HIGH | `0x03` | `0xFC` |
| MEDIUM | `0x05` | `0xFA` |
| LOW | `0x09` | `0xF6` |

These values match the earlier fan-setting observations. In contrast, the HEAT-mode
cycle at 23°C produced `0xF6`, `0xE6`, `0xEA`, and `0xF2` for AUTO, HIGH, MEDIUM, and
LOW. This supports fan-speed correlation in both tests, but shows that byte 3 is
mode-dependent; do not treat either set of values as a universal standalone fan code.

A full-round capture then swept fan speed through FAN, COOL, AUTO, and HEAT modes, with
no setpoint changes and about five seconds between button presses. The boot and
power-on traffic is excluded here; the aligned candidate frame
`7F 22 03 <byte3> 02 5F 7E 7F` was compared. For FAN, COOL, and AUTO, the reported
sequence was LOW → AUTO → HIGH → MEDIUM → LOW. HEAT has four observed values, beginning
at AUTO and ending at LOW:

| Operating mode | Decoded byte 3 in fan-setting order |
|---|---|
| FAN | `09` → `0B` → `03` → `05` → `09` |
| COOL | `BC` → `BD` → `B9` → `BA` → `BC` |
| AUTO | `79` → `7B` → `73` → `75` → `79` |
| HEAT | `F6` → `E6` → `EA` → `F2` (AUTO → HIGH → MEDIUM → LOW) |

Within each mode, only byte 3 changes in this candidate frame as fan speed is cycled;
the other seven decoded bytes remain `7F 22 03` (prefix), `02 5F 7E 7F` (suffix).
The first FAN-mode HIGH sample was split across an 8-byte receive window, but the
adjacent bytes reconstruct it as the same candidate frame with byte 3 `03`. The full
round trip therefore reinforces that byte 3 tracks fan selection, with a different
encoding for each operating mode. It is still an observed field correlation, not a
validated bit-level decoder.

An off transition also produced the decoded sequence `7F 24 03 7F 14 03 ...` (captured
bytes begin `80 DB FC 80 EB FC ...`) in two captures. Treat this as a repeatable observed
signature, not a fully decoded power-off command.

The off signature and other historical observations remain only partially decoded; do
not use this note alone as a specification for transmitting commands.

## Compatibility With the Current ESPHome Decoder

The current vendored decoder in `components/fujitsu_climate/FujiHeatPump.cpp` updates
controller state only for frames addressed to its configured secondary address `0x21`.
The repository's earlier deployed-board capture documents a different frame addressed
to that controller: source `0x20` → destination `0x21` (see
`docs/session-5A-checkin-2026-10-03.md`). The candidate frames in the fan-cycle captures
decode as source `0x7F` → destination `0x22`; the engine stores those as other frames
and does not use them to update climate state or the received-frame timestamp.
In the full-round `raw.log`, all 25 decoded occurrences with destination `0x22` are
other frames, and there are no decoded frames addressed to `0x21`. Thus, for this log,
the component's "never seen" status reflects that it received no frames for its
configured secondary address; it does not mean the UART received no bytes.

The ESP32 TX wire was physically disconnected during these tests. The secondary
implementation listens for primary frames and must transmit replies to complete its
presence/login exchange, so this setup cannot establish that the ESP32 joined the bus.
It is plausible that the observed traffic/addressing differs because the ESP32 never
joined, but this alone does not explain the decoded destination `0x22`: the documented
primary address is `0x20`, and the candidate frame's role/boundary is not proven. The
offline capture therefore cannot tell us what destination or frame sequence would be
seen after a secondary successfully joins.

The current field decoder also interprets byte 3 with separate mode and fan masks. For
example, it maps candidate byte `0xF2` to mode value 1 (FAN), even though this value was
labeled HEAT in the capture. The fan-cycle captures establish that these candidate
frames carry state correlated with selected mode and fan speed, but do not establish
that they are the frames the ESPHome climate entity should consume. Do not change the
live address/decoder based on this evidence alone. Any test that reconnects TX should
first be planned around the existing reply/login behavior and performed with a way to
disconnect or power down the ESP32 immediately if the bus behaves unexpectedly.

## Experimental HA State Observation

The component separately parses the continuous decoded byte stream into `0x7F`-delimited
messages (`FujiPassiveStream.h`; see the stream-structure section of
`docs/protocol-investigation-status-2026-10-31.md`), without changing the vendored `0x21`
login/reply decoder:

- A setting message `7F 22|12 03|13 <value> 02 5F 7E` (remote message or unit echo)
  updates the climate entity from the observed controller selection, using only the
  values mapped from the captures in this note. The echo carries the same state and does
  not republish.
- A power-off message `7F 24|14 03` sets the climate entity to Off and retains its last
  known setpoint and fan setting. A status message whose running flag (`7F 28|18 13|17 ..`,
  byte 2 bit 2) falls from running to idle also sets Off, in case the power-off message
  was lost.
- Boot limitation: mode, fan and setpoint are only sent when the remote changes them, so
  after a reboot HA stays unknown until the next remote button press.

`tests/passive_stream_test.cpp` replays all ten captures through the parser, including
the TX-enabled follow-up. Its remote setting sequence decodes to `F2 D2 B2 92 09 0B
03 05` (HEAT 23→20°C, then FAN LOW → AUTO → HIGH → MEDIUM), followed by one remote
power-off message and its unit echo. The ESPHome log reports the corresponding HA state
changes. Despite the firmware's `Control enabled: YES -- LIVE` setting, there are no
`-->` reply logs; this capture does not demonstrate a secondary login or bus transmission.
See the follow-up analysis in
[`docs/protocol-investigation-status-2026-10-31.md`](docs/protocol-investigation-status-2026-10-31.md).

This is an experimental, receive-only state path. It can show the wired controller's
selected mode, fan setting, and known setpoint in HA before the ESP32 has joined as a
secondary. It does **not** prove that the indoor unit accepted the command or that the
compressor is actively heating/cooling. The current-temperature field remains unknown
until the existing `0x21` protocol path provides it. Unmapped byte-3 values are ignored
rather than guessed; for partially mapped observations, HA retains the last known
field. The component's Bus Status/Bus Alive signals continue to mean that traffic for
the configured `0x21` secondary was received.

The mapping currently covers FAN values `03`, `05`, `09`, `0B`; AUTO values `73`,
`75`, `79`, `7B` at 23°C; COOL values `B9`, `BA`, `BC`, `BD` at 23°C and `C4` at
24°C; and HEAT values `12`, `52`, `72`, `92`, `B2`, `D2`, `E6`, `EA`, `F2`, `F6`.
The 24°C COOL value does not update fan mode because its fan mapping has not been
confirmed. No commands are transmitted by this observation path.