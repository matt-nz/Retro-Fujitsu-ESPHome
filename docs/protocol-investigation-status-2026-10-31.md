# Fujitsu Protocol Investigation Status — 31 October 2026

## Current state

- Working branch: `debug/uart-receive-logging`.
- Current implementation/evidence commit before this note: `d6e293d`.
- The ESPHome component has an experimental receive-only observer for patterns seen
  in the captures. It scans decoded bytes across UART read boundaries and can publish
  observed wired-remote mode, some fan settings, and supported setpoints to the HA
  climate entity. It does not change the existing secondary reply/login decoder or
  transmit commands.
- The observer reflects the controller's selected settings, not proof the indoor unit
  accepted them or that heating/cooling is active. Room temperature remains unknown
  until the existing secondary protocol path provides it. Bus Alive continues to
  indicate traffic addressed to the configured secondary (`0x21`), not any RX activity.
- The parser harness and offline signature replay passed for the observed mappings,
  split UART reads, and Off detection. A full ESPHome firmware build was not run
  locally; compile and flash in Home Assistant before evaluating the HA state.

## Capture findings

The nine sanitized logs under `../captures/matt-captures/` contain raw UART reads at
500 baud, 8E1. Each bus byte is inverted (`XOR 0xFF`) in the current decoder. Logged
8-byte UART reads are not necessarily protocol-frame aligned.

The strongest repeated candidate is:

```
7F 22 03 <value> 02 5F 7E 7F
```

Its byte-3 values correlate with the wired remote's recorded mode, fan, and—in Heat
and some other captured conditions—setpoint changes. The repeatable Off signature is
`7F 24 03 7F 14 03`. These are observed signatures, not a fully validated protocol
specification. The role of address `0x22` is unknown; do not assume it is the outdoor
unit or that it is addressed to the ESP.

The value mappings currently implemented are documented in
[`protocol-matt.md`](../protocol-matt.md). Unknown values are ignored rather than
guessed. No captured evidence yet establishes the complete secondary login exchange,
all operating-state fields, or safe command transmission.

## Sniffer sketch comparison

[`../reference/fujitsu-sniffer-main.cpp`](../reference/fujitsu-sniffer-main.cpp) is
included as a reference from the separate `FujiHeatPump` project (trailing whitespace
was removed). Its upstream MIT license is included alongside it. It shares
the 500-baud 8E1 UART setup and byte inversion. Its active loop reads and prints
individual bytes; it does not call `printSniffedFrame()`, so it does not actually
assemble or decode frames. Its byte-3 nibble temperature/fan and byte-4 power
interpretations do not match the candidate frames and labeled captures here. Treat it
as evidence of the prior sniffing setup, not as a decoder to reuse.

## Independent analysis request

When providing these materials to another protocol analyst, ask them to:

1. Reconstruct frame boundaries from all raw RX bytes, accounting for partial reads
   and arbitrary log chunk boundaries.
2. Independently compare repeated frames to the operator-labeled mode, fan, setpoint,
   and power changes. Distinguish demonstrated correlations from inferred semantics.
3. Assess what can and cannot be inferred about source/destination roles, especially
   `0x22`, and identify evidence of a secondary login/response exchange.
4. Review the current receive-only mapping for unsupported assumptions and propose the
   smallest next passive observation that would discriminate between competing
   interpretations.
5. Do not recommend transmitting or changing addresses until the frame roles and
   reply timing are supported by evidence.

Materials: this note, [`../protocol-matt.md`](../protocol-matt.md), all nine logs in
[`../captures/matt-captures/`](../captures/matt-captures/), the current
`components/fujitsu_climate/FujiHeatPump.cpp`, and the included sniffer sketch.

## Follow-up analysis — raw replay and related projects

The nine `matt-captures` logs were replayed by extracting each `RX raw` read, joining
the reads in file order (so UART read boundaries are not treated as frame boundaries),
then inverting each byte and searching for the two observed signatures. This recovered
24,585 bytes, 79 occurrences of the candidate 8-byte frame, and 14 occurrences of the
6-byte Off signature. These are signature occurrences, not 79 or 14 independent button
presses or protocol transactions.

| Capture | Candidate frame occurrences | Observed byte-3 values | Off signature occurrences |
|---|---:|---|---:|
| `auto-heat.log` | 8 | `52 72 09 BC 79` | 2 |
| `cold.log` | 16 | `52 72 09 BC 79 C4` | 4 |
| `fan_changes.log` | 4 | `F6 E6 EA F2` | 0 |
| `heat_20_22_18.log` | 8 | `09 92 B2 D2 72 52` | 2 |
| `hot-fan-cold-auto.log` | 13 | `52 72 09 BC 79` | 3 |
| `on_off.log` | 1 | `09` | 1 |
| `raw.log` | 28 | `F6 E6 EA F2 0B 03 05 09 BC BD B9 BA 79 7B 73 75` | 1 |
| `running_standby.log` | 0 | — | 0 |
| `startup.log` | 1 | `F2` | 1 |

This independently confirms that byte 3 varies across the operator-labeled mode, fan,
and setpoint captures while the candidate prefix/suffix stay constant. It does not
establish a bit-field layout, checksum, bus address meaning, or that the candidate is
the frame the ESP32 should transmit. The observed `0x20` HEAT setpoint step and the
mode-specific fan value tables are still correlations; the next useful capture is a
HEAT fan sweep repeated at a second fixed setpoint, with the physical controller's
model and each button change timestamped. That tests whether setpoint and fan effects
combine independently or interact in byte 3.

### Public-project comparison (searched 31 Oct 2026)

- [`unreality/FujiHeatPump`](https://github.com/unreality/FujiHeatPump) is the upstream
  3-wire remote-interface library already vendored by this project. Its README describes
  primary/secondary controller roles, and warns that the primary's one secondary-presence
  check occurs about four seconds after power-on; its example replies after 50–60 ms.
  This supports the implemented protocol family and highlights why login/reply timing
  matters, but does not independently confirm compatibility with this AOT30L/ART30LUAK
  installation. This repository's measured timing takes precedence over the generic
  upstream delay.
- [`Omniflux/esphome-fujitsu-halcyon`](https://github.com/Omniflux/esphome-fujitsu-halcyon)
  targets Fujitsu AirStage-H / Halcyon units on the three-wire RWB bus; its documented
  example family includes ACY-series equipment. It is a useful architectural comparison,
  but no AOT30L/ART30LUAK match or shared byte-level encoding was found.
- [`danirebollo/FujiWired2WiFi`](https://github.com/danirebollo/FujiWired2WiFi) describes
  a wired half-duplex UART Fujitsu interface and names the ACY100UIA-LL as an example.
  It likewise provides no evidence that its frame format applies to this older unit.
- GitHub searches for `AOT30L`, `ART30LUAK`, `AR-3TA3`, Fujitsu LIN/UART, and related
  protocol terms did not surface another independent decoder for this exact model
  family. This is evidence that the present captures are unusually valuable, not proof
  that no other implementation or service documentation exists.

### Home Assistant control boundary

The current HA climate `control()` path uses the vendored secondary-controller
protocol: it records a requested power/mode/setpoint/fan update and sends it as a
reply to a frame addressed to the configured secondary (`0x21`). The candidate
`0x22` observer is receive-only and only reports wired-controller selections; it is
not a safe basis for inventing a transmit frame. The climate entity also exposes
requested-versus-decoded state synchronization so an eventual live command can be
checked against the unit's subsequent decoded state.

This paragraph's original recommendation (exercise the `0x21` path next) is superseded
by the stream-structure finding below: the stream in these captures does not look like
the 8-byte upstream frames that path assumes. Keep the HA control-enable switch as the
stop/disable mechanism, and do not transmit anything synthesized from these captures.

## Stream structure finding (`startup.log` and TX-enabled follow-up)

The log contains no `-->` lines, so the ESP32 never transmitted even with TX enabled.
The upstream engine only replies to an 8-byte read whose decoded destination is `0x21`;
that never occurs because the read chunks are not the protocol's frames.

Joining the reads and splitting the inverted stream at each `0x7F` shows variable-length
messages, not 8-byte frames (the original nine logs and the TX-enabled follow-up agree):

| Message (inverted bytes) | Observed when |
|---|---|
| `7F 28 13 08` then `7F 18 13 48 80` | Repeating poll/response pair while the unit is off |
| `7F 28 17 08` then `7F 18 17 48 80` | Same pair while the unit is on (byte 2 `13`→`17`) |
| `7F 22 03 <v> 02 5F 7E`, then `7F 12 03 <v> 02 5F 7E` | A wired-remote setting change, then the same body with `12` |
| `7F 28 17 0A` | Once or twice around a state change |
| `7F 24 03`, then `7F 14 03` | Power off |

In `startup.log` and `on_off.log` the on/off sequence is consistent: the unit starts
on `28 13`; the `22 03 <v>` message and its `12 03 <v>` echo appear; one `28 17 0A`
follows; polls become `28 17 08`. After `24 03` / `14 03`, polls return to `28 13`.
So byte 2 of the poll tracks on/off in these logs.

Working hypothesis (not proven): the high nibble of the second byte is a direction
(`2` = wired remote to unit, `1` = unit reply) and the low nibble is a message class
(`8` poll, `2` settings, `4` power). If so, `0x22` is not a bus address, and this
unit (AOT30L series) uses a different protocol from the `0x20`/`0x21` frames in
`PROTOCOL.md`. No checksum has been identified; `5F` stays constant while byte 3 changes.

Consequences:
- The vendored secondary-controller engine does not match this bus and cannot log in.
  Do not use it to transmit on this bus.
- `FujiPassiveStream.h` now splits the stream on `0x7F`; host replay tests cover the
  original captures and the TX-enabled follow-up. This passive parser does not transmit.

The TX-enabled follow-up (`../captures/matt-captures/tx-enabled-new-build.log`) was made
with firmware 4A.12b-rc1. About 30 seconds after logging began, the remote was turned on
in HEAT at 23°C, stepped down to 20°C, switched to FAN, cycled through LOW → AUTO →
HIGH → MEDIUM, then turned off. Offline replay reconstructs the remote-to-unit setting
values `F2 D2 B2 92 09 0B 03 05`, followed by one power-off message and its unit echo.
These match the operator's actions. The HA log also reports the matching mode, setpoint,
and fan changes; room temperature remains unavailable.

Despite `Control enabled: YES -- LIVE`, this capture contains no `-->` replies and no
8-byte decoded reads with destination `0x21`. It therefore provides no evidence of a
secondary login or transmission. The log predates the passive stream parser integration
in firmware, so the next software validation is to build and flash the current branch,
then verify the same HA state sequence and confirm that the new parser remains receive-only.
Only after that should secondary joining be investigated separately; do not infer a
transmit frame from these captures.
