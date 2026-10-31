# Fujitsu Heat Pump Protocol Notes

> **Provenance and status.** These notes were first written by an AI assistant from live
> captures of this installation (indoor unit ART30RL, wired remote part 9364911014, an
> older remote that does not display a room temperature). On 31 Oct 2026 they were audited
> against the committed captures in `captures/matt-captures/`; corrections are folded in
> below, and statements that no committed capture supports are marked as such. The open
> source Retro-Fujitsu project this repository's code comes from (`PROTOCOL.md`,
> `docs/session-*`) targets a similar but different unit, and its 8-byte `0x20`/`0x21`
> frames have not been seen on this bus. Treat everything here as observed correlation,
> not a validated specification.

## Capture Format and Setting Message

The ESPHome log records UART reads of up to 8 bytes; read boundaries do not line up with
the protocol. Each logged byte inverts (`XOR 0xFF`) to the bus data below. The UART is
configured for 500 baud, 8E1.

After inversion the stream is a series of variable-length messages that each start with
`0x7F` (see the stream-structure section of
[`docs/protocol-investigation-status-2026-10-31.md`](docs/protocol-investigation-status-2026-10-31.md)
for the full list). Most tables below use the setting message:

```
Decoded:      7F 22 03 <value> 02 5F 7E      remote -> unit
Decoded:      7F 12 03 <value> 02 5F 7E      unit echo of the same state
Captured bus: 80 DD FC <~value> FD A0 81     (remote message)
```

Earlier versions of this note showed a trailing `7F` as part of an 8-byte frame; it is the
start of the next message. `<value>` is the only byte that changes across all the setting
messages in the captures; it follows the selected mode, fan speed and setpoint. The second
byte's `0x22`/`0x12` is probably a direction and message class rather than an address
(unproven), and its third byte is `03` or `13` for reasons not yet understood. No checksum
has been identified: `02 5F 7E` is constant. The checksum equations in earlier versions of
this note did not fit these messages and should not be used. Do not transmit based on this
note.

## Example Messages

### Fan Low (operator-labeled capture)
```
Decoded:      7F 22 03 09 02 5F 7E
Captured bus: 80 DD FC F6 FD A0 81
```

Earlier versions also listed a "17°C setpoint" example (`value` `0x32`) and an "other
observed frame" (`7F 28 13 48 80 7F 28 17`). Neither appears in the committed captures.
The first is only what the HEAT setpoint pattern below would predict; the second does not
match any message shape and was probably a corrupted read.

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
| 24°C (not in committed captures) | `0x12` | `0xED` |

The 18→19→18°C test was repeated while powered on and produced `0x52`→`0x72`→`0x52`.
The 20→22→18°C test (`heat_20_22_18.log`) produced `0x92`→`0xB2`→`0xD2`→`0xB2`→`0x92`
→`0x72`→`0x52`, one value per degree. A later 23→24→23°C HEAT test was reported to produce
`0xF2`→`0x12`→`0xF2` (the `+0x20` step wrapping at `0xFF`), but no committed capture
contains `0x12`, so 24°C and the wrap are unverified. Setpoints other than 18–23°C in
HEAT have not been captured. These observations apply to HEAT; they do not establish that
every operating mode uses the same layout.

In COOL mode, a separate 23→24→23°C test produced decoded byte 3 values
`0xBC`→`0xC4`→`0xBC` (captured bytes `0x43`→`0x3B`→`0x43`). This confirms the byte
tracks the setpoint in COOL mode too, but the observed one-degree change is `+0x08`,
not the `+0x20` seen in HEAT. A combined HEAT/Cool capture at 23°C showed `0xF2` in
HEAT and `0xBC` in COOL, confirming the mode dependence at the same setpoint.

The `hot-fan-cold-auto.log` capture steps the HEAT setpoint 18→19→18°C (`0x52`→`0x72`
→`0x52`) and then cycles modes twice. The remote setting values, in order, were:

| Selected mode | Decoded `<value>` | Captured bus byte |
|---|---:|---:|
| HEAT (18°C) | `0x52` | `0xAD` |
| FAN | `0x09` | `0xF6` |
| COOL | `0xBC` | `0x43` |
| AUTO | `0x79` | `0x86` |
| HEAT (18°C) | `0x52` | `0xAD` |

Earlier versions of this note listed HEAT as `0xF2` here; that value is HEAT at 23°C
(see `raw.log`), not what this log shows. The COOL and AUTO setpoints were not recorded;
the `23°C` attached to them in the code's mapping is an assumption (supported for COOL by
the 23→24→23°C test). `0x09` also appeared during a fan-speed test labeled LOW, so
`<value>` is not a standalone mode field.

A dedicated fan-speed capture was then made with the unit held in HEAT at 23°C, changing
the wired controller's fan setting LOW → AUTO → HIGH → MEDIUM → LOW. Four successive
setting messages changed decoded byte 3 as follows:

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
LOW → AUTO → HIGH → MEDIUM → LOW. The four intervening setting messages produced:

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
power-on traffic is excluded here; the setting message
`7F 22 03 <byte3> 02 5F 7E` was compared. For FAN, COOL, and AUTO, the reported
sequence was LOW → AUTO → HIGH → MEDIUM → LOW. HEAT has four observed values, beginning
at AUTO and ending at LOW:

| Operating mode | Decoded byte 3 in fan-setting order |
|---|---|
| FAN | `09` → `0B` → `03` → `05` → `09` |
| COOL | `BC` → `BD` → `B9` → `BA` → `BC` |
| AUTO | `79` → `7B` → `73` → `75` → `79` |
| HEAT | `F6` → `E6` → `EA` → `F2` (AUTO → HIGH → MEDIUM → LOW) |

Within each mode, only byte 3 changes in this message as fan speed is cycled;
the other six bytes remain `7F 22 03` (prefix) and `02 5F 7E` (suffix).
The first FAN-mode HIGH sample was split across an 8-byte receive window, but the
adjacent bytes reconstruct it as the same setting message with byte 3 `03`. The full
round trip therefore reinforces that byte 3 tracks fan selection, with a different
encoding for each operating mode. It is still an observed field correlation, not a
validated bit-level decoder.

Every power-off in the captures (15 across eight logs) produced the two messages
`7F 24 03` (remote) and `7F 14 03` (unit echo); captured bytes begin `80 DB FC 80 EB FC`.
While the unit is on, polls are `7F 28 17 08` and replies `7F 18 17 48 80`; after power-off
they return to `13`. This is a repeatable observed signature, not a fully decoded command.

The off signature and other historical observations remain only partially decoded; do
not use this note alone as a specification for transmitting commands.

## Compatibility With the Vendored ESPHome Decoder (superseded)

Earlier versions of this note analysed these captures with the vendored decoder's 8-byte
frames, reporting "source `0x7F` -> destination `0x22`" and 25 "other frames" in `raw.log`.
That was an artefact: the bus does not carry 8-byte frames. Consequences:

- The vendored `0x20`/`0x21` secondary-login engine never sees a frame addressed to `0x21`
  on this bus, so it never replies. No capture contains a `-->` reply line.
- The `0x20` -> `0x21` frames in `docs/session-5A-checkin-2026-10-03.md` come from the other
  project's unit and have not been seen here.
- Bus Status / Bus Alive originally depended on `0x21` traffic and would have stayed at
  "Never Seen" on this bus; they now also count recognised passive messages.
- Whether the ESP32's TX was connected varied by capture: the operator reported TX
  disconnected for the fan-speed captures; `startup.log` and `tx-enabled-new-build.log`
  were made with TX enabled; the others were not recorded.

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

`tests/passive_stream_test.cpp` replays all eleven captures through the parser, including
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
compressor is actively heating/cooling. No room temperature has been found on this bus (the
remote does not display one, and no message byte varies with room conditions), so the
current-temperature field stays unknown; use an external temperature sensor. Unmapped
`<value>` bytes are ignored rather than guessed; for partially mapped observations, HA
retains the last known field. Bus Status/Bus Alive report "alive" while recognised
messages arrive (timeout 4 s); the climate entity still follows the passive state because
no `0x21` frame is ever received.

The mapping currently covers FAN values `03`, `05`, `09`, `0B`; AUTO values `73`,
`75`, `79`, `7B` at 23°C; COOL values `B9`, `BA`, `BC` for HIGH/MEDIUM/LOW fan at
23°C, `C4` at 24°C with fan unknown, and `95`, `9D`, `A5`, `AD`, `B5`, `BD`, `C5`,
`CD`, `D5`, `DD`, `E5`, `ED`, `F5` for AUTO fan at 18-30°C; and HEAT values `52`, `72`, `92`, `B2`, `D2`,
`E6`, `EA`, `F2`, `F6`. The COOL/AUTO sweep is recorded in
[`captures/matt-captures/cold-test.log`](captures/matt-captures/cold-test.log) and
replayed by `tests/passive_stream_test.cpp`. The `T31` label was not a 31°C setting
change, so no 31°C behavior is established. HEAT `12` (24°C) is deliberately not mapped
because no committed capture contains it. The isolated 24°C COOL value `C4` does not
update fan mode because that fan mapping has not been confirmed. No commands are
transmitted by this observation path.