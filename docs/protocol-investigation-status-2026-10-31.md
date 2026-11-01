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

Materials: this note, [`../protocol-matt.md`](../protocol-matt.md), the original nine logs in
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
messages, not 8-byte frames (the original nine logs and both later captures agree):

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
secondary login or transmission. Do not infer a transmit frame from these captures.

## Labeled COOL setpoint sweep (`cold-test.log`)

This follow-up was captured with the passive parser firmware, with labels entered before
the remote changes. In COOL with AUTO fan, the 18→30°C sweep produced these setting
values in order:

```
18-30°C: 95 9D A5 AD B5 BD C5 CD D5 DD E5 ED F5
```

Each degree changes the byte by `+0x08`; `BD` at 23°C matches the already observed
COOL/AUTO value. The remote's COOL setpoint range is 18-30°C; the `T31` label was not
a setting change and does not indicate a 31°C option. The 28°C step follows the
sequence (`E5`) and is consistent with the reported setpoint despite the operator's
concern about that part of the test. This supports decoding the observed COOL/AUTO
sequence across the remote's full range. Other COOL fan settings remain mapped only at
23°C, and the isolated 24°C value `C4` still has unknown fan. Host replay tests now
include this capture and verify the full sequence. The capture ends with an Off message,
but it has no Off label, so operator intent for that final transition is not established.

This capture adds passive state evidence only; it contains no outgoing `-->` reply and
does not demonstrate secondary login.

### Firmware verification (build compiled 31 Oct 2026 21:40, with the passive parser)

This log was made with the build that includes the passive parser (its debug lines
`Observed controller ON state` / `Observed controller OFF` come from that code). The ESP32
was booted at 21:51:20; the first bus traffic arrived at 21:51:43, about 23 s later (the
unit powering up); the remote then drove these changes. HA's climate entity followed every
step:

| Time | Remote action | Setting byte | HA climate result |
|---|---|---|---|
| 21:52:12 | On, HEAT 23°C | `F2` | HEAT, 23°C, fan LOW |
| 21:52:23 | 22°C | `D2` | HEAT, 22°C, fan LOW kept |
| 21:52:24 | 21°C | `B2` | HEAT, 21°C |
| 21:52:26 | 20°C | `92` | HEAT, 20°C |
| 21:52:29 | FAN mode | `09` | FAN_ONLY, fan LOW, no setpoint |
| 21:52:35 | Fan AUTO | `0B` | FAN_ONLY, fan AUTO |
| 21:52:39 | Fan HIGH | `03` | FAN_ONLY, fan HIGH |
| 21:52:45 | Fan MEDIUM | `05` | FAN_ONLY, fan MEDIUM |
| 21:52:50 | Off | `24 03` / `14 03` | OFF, last fan setting kept |

Each state published once (the unit's echo did not republish). Still not working or not
verified:
- ESPHome logged `Current Temperature: 0.00°C` even though the component's own debug line
  says `current=nan`. Check in HA whether the current temperature shows as unknown or as 0°C;
  if 0°C, the entity needs to be fixed so it does not report a false reading.
- HA action stays `IDLE` while the unit is on, by design (the remote's selection is not
  proof the compressor is running).

### Message exchange details seen in this log

- Each remote change produces an exchange: `22 03 <v>` (remote), `12 03 <v>` (unit echo),
  `28 17 0A` (poll with `0A`), then `12 03 <v>` again. Most changes were then repeated as a
  second round with the second byte's bit 4 set: `22 13 <v>`, `12 13 <v>`, `28 17 0A`,
  `12 13 <v>`. Only the power-on (`F2`) and the switch to FAN mode (`09`) had just the first
  round, so the meaning of the `03` / `13` difference is not yet understood.
- At bus start-up the unit sends one short message `7F 12 01` before the normal poll cycle
  (also at the start of `startup.log`). It is not decoded and the parser reports it as
  unknown; it may be a unit-ready announcement.
- The poll replies in every capture are the fixed `7F 18 13|17 48 80`, so no room temperature
  or other measurement has appeared on the bus yet.

Next validation: capture the same sequence with the unit already running at ESP32 boot, to
confirm that HA stays unknown until the next remote press, and with the mains power-up
captured from the very first byte to see what the `7F 12 01` start-up message is.

## Capturing one setting at a time

The ESPHome log already carries millisecond timestamps from the computer's clock, so each
remote press can be matched to the messages it produced. Two helper scripts in
[`../tools/`](../tools/) make this easy:

```
esphome logs retrofujitsu.yaml 2>&1 | python3 tools/capture_labels.py captures/matt-captures/my-test.log
python3 tools/decode_timeline.py captures/matt-captures/my-test.log
```

Without the `esphome` CLI installed (for example when ESPHome only runs as a Home Assistant
add-on), `pip install aioesphomeapi` and use `tools/api_log_capture.py` instead; it reads the
device's logs from the native API and applies the same labels. It is untested against a real
device so far:

```
python3 tools/api_log_capture.py <device-ip> captures/matt-captures/my-test.log [--key API_KEY]
```

If neither is possible, copy the add-on's log view into a file and add label lines by hand,
in the form `[HH:MM:SS.mmm][LABEL] heat 24`, using the same clock as the log's timestamps
(note the HA host's clock may differ from yours). `decode_timeline.py` accepts those.

While capturing, press a remote button, then type a short label (for example `heat 24` or
`fan high`) and Enter. `capture_labels.py` writes the label into the log with the same
clock. `decode_timeline.py` prints every non-routine bus message in time order, grouped
under each label, with the unit's running/idle changes marked and discontinuities shown;
`--all` also prints the routine poll/reply pairs. It prints raw bytes and does not
interpret them.

Tips:
- Wait at least five seconds between presses so the two-round exchange for one change does
  not run into the next.
- Start with the unit idle, turn it on, run the whole experiment, and turn it off. The
  running/idle marker and the `24 03` / `14 03` power-off messages then bracket the run
  without any extra labels.
- Without a keyboard, known values can act as markers instead: a fan change decodes
  unambiguously (for example in FAN mode `03` is HIGH and `09` is LOW). Bracket each test
  with one so the log can be split afterwards. Typed labels are cleaner, because a marker
  press is itself a state change.
- Useful sets to capture: every setpoint 16–30°C in each mode, DRY mode, and any other
  remote buttons (swing, economy, timer), one change per label.

## Setting-value bit-field model (hypothesis, 31 Oct 2026)

All previously labeled (mode, setpoint, fan) values, plus the 18-30°C COOL/AUTO sweep
above, fit one model with no mismatches. Write the setpoint as `T` and the fan setting as a 3-bit code
(`high` = 1, `medium` = 2, `low` = 4, `auto` = 5); then the setting byte is

```
value = ((T << 3 | fan) << shift | marker) & 0xFF
  COOL        shift 0  marker 0b0
  FAN, AUTO   shift 1  marker 0b1     (FAN carries no setpoint: T = 0)
  HEAT        shift 2  marker 0b10
```

Examples: COOL 23°C low fan `BC` = `10111 100`; AUTO 23°C low `79` = `0111 100 1`; HEAT
23°C low `F2` = `111 100 10`. Fan only changes the 3-bit field, and the field moves left
with the mode, which is why the same fan setting gives different bytes per mode.

The HEAT 18–22°C setpoint capture values `52`, `72`, `92`, `B2`, and `D2` match LOW fan
under this model, consistent with the previously observed LOW setting. The observer now
reports LOW for that captured sequence. Other unobserved combinations remain unmapped.

This also predicts values that no committed capture contains but the earlier AI notes
reported: HEAT 24°C = `12` and 17°C = `32`. That is weak independent support for those
notes, not confirmation, so `0x12` stays unmapped in the parser.

Consequence and test: the shift pushes setpoint bits out of the byte. In HEAT only the low
three bits of `T` survive, so 16 and 24°C both give `12`, 17 and 25 give `32`, and so on up
to 22 and 30 (`D2`). COOL (16–30°C) and AUTO have no collisions. If the remote sends the
full setpoint, the missing bits must be somewhere else in the message (for example the
`03`/`13` byte), or the unit infers them. Capturing HEAT at 24–30°C, and 16–17°C, and
comparing every byte of the setting messages tests this directly. Predicted values for
COOL 16–30°C at low fan: `84 8C 94 9C A4 AC B4 BC C4 CC D4 DC E4 EC F4`.

The model is a description of the labeled observations, not a validated spec. Quiet fan,
DRY, and any other remote settings are unmodelled.

## Logic-analyser decode of the start-up capture (1 Nov 2026)

`captures/matt-captures/start-up.sr` (1 MHz, 23.3 s) was decoded from raw samples with
`tools/decode_logic_capture.py` and validated; the full results are in
[`../captures/matt-captures/start-up-notes.md`](../captures/matt-captures/start-up-notes.md). Headline findings:

- Two transmitters with different bit clocks: the remote at **1980.8 µs** per bit (504.8 baud) and the
  unit at **2055.6 µs** (486.5 baud), neither 500 baud. The second message byte's high nibble (`2`
  remote, `1` unit) identifies the sender in all 142 messages.
- Bytes are 11 cells: start, 8 data, a ninth cell, stop. The ninth cell is not parity (392 of 659 bytes
  differ from even parity); it follows a fixed per-position sequence `0 0 1 1 0 1 0` for both senders.
- 22 stop cells are low, exactly bytes 2 and 3 of every setting message (both senders); those bytes are
  12 and 13 cells long. A fixed 500 baud 8E1 receiver still returns all 659 bytes intact, flagging 392
  parity and 22 framing errors.
- Timing: poll every ~247 ms, reply 1.7–10 ms after the poll ends, setting echo 22–28 ms after the setting.
- A 500 baud 8E1 UART cannot reproduce the remote's waveform, so any transmit experiment needs
  cell-level control of the line. Do not transmit from this capture's evidence alone.
- Unexplained: a unit reply that stalled for 1.01 s and ended with a lone `04`; the extra stop-cell low
  pulse; the `03`/`13` and `08`/`0A` variants.

## Next logic-analyser session checklist (1 Nov 2026)

Goal: find out whether any byte or cell besides the setting value varies with the setting, and
whether the unit sends anything without the remote. Capture on D1 (1 MHz for the short runs; 50 kHz
for long ones) with the same wiring as `start-up.sr`, one file per step, and decode each with
`python3 tools/decode_logic_capture.py <file>.sr --messages`. Write the time of every press on paper
(the analyser file has no labels), and keep presses at least 5 s apart.

1. **Reference run.** Power up the unit, press ON in HEAT, step the setpoint one degree at a time
   18→23 and back, with fan AUTO. Compare every byte and cell of each setting message, not only the
   value byte. Expected if there is no checksum: only the value byte changes.
2. **Fan and mode sweep.** At a fixed setpoint, change fan LOW/MEDIUM/HIGH/AUTO, then each mode (COOL, DRY,
   FAN, AUTO, HEAT). Note whether the extra cells (ninth cell, stop-cell low pulse) ever change.
3. **`03` vs `13`.** Make one single press, then a press-and-hold of the same button. Record which one
   produces the second round with `22 13`.
4. ~~**Remote disconnected.**~~ Done by the operator: with the wired remote unplugged the unit puts
   no traffic at all on RX. So the unit never speaks first; the remote owns the poll cycle and the unit
   only replies (and echoes). Any transmitter would have to take the remote's role (poll and settings),
   not join as a quiet second controller, and cannot be tested with the remote removed.
5. **Idle soak (use a low sample rate).** Sample rate is the thing to cut, not the length. At 1 MHz
   ten minutes is 600 million samples, which PulseView holds in RAM; the compressed `.sr` would be small
   but the session would not. A bit is 2 ms wide, so 50 kHz (20 µs resolution) is plenty: ten minutes
   is 30 million samples (about 30 MB in memory). I replayed `start-up.sr` decimated to 100, 50,
   and 20 kHz; `decode_logic_capture.py --check` still passed at all three (bit times within 0.1%
   of the 1 MHz values, same 659 bytes, 392 parity and 22 framing flags). If memory is still a
   worry, take two or three five-minute files instead. Anything the idle soak is looking for
   (`0A` polls, changed replies, a stalled reply) is a whole-message event, so the lower rate
   does not hide it. Leave the unit running 10 minutes with no presses to see whether `08` becomes
   `0A`, a reply byte other than `48 80` appears, or the 1 s reply stall repeats.
6. **Room temperature (low priority).** Existing data already says the reply carries none: across all
   ESP logs from 31 Oct (warm day, 13:22–21:53; 3,000 replies) and `start-up.sr` from 1 Nov (cooler
   morning, 105 replies), the unit's reply is always `7F 18 13|17 48 80`; the only exception is the
   stalled `7F 18 17` + `04` at 14.13 s. Polls vary only `08`/`0A`. So a temperature that differed
   between the two days would have shown up as a changing byte and did not. Warming the remote
   deliberately is only worth doing if the `04` or `0A` cases turn out to follow temperature (check
   whether they recur in the idle soak).

Decision after the session: if only the value byte and fixed cell patterns vary, the next step is a
bench transmitter on a spare line checked against the remote's capture at microsecond level, before
any connection to the heat pump. The existing one-shot UART transmit experiment stays unused until then
because a 500 baud 8E1 UART cannot reproduce the remote's waveform.

## Cell-position decode of setting packets and checklist progress (1 Nov 2026)

Four more logic-analyser captures (`heat.sr`, `fan-mode.sr`, `eco.sr`, `idle.sr`) show that the setting message
is a bit-serial packet with fixed field positions: round flag, mode, fan, setpoint and ECO each occupy fixed
cells, and no other cell varies in any of 149 captured packets (no checksum is visible). Details, the field
table, timing and open items are in
[`../captures/matt-captures/settings-captures-notes.md`](../captures/matt-captures/settings-captures-notes.md);
`python3 tests/logic_capture_test.py` checks it.

This supersedes the "setting-value bit-field model" above (its shift-by-mode was an artefact of UART framing)
and explains the ninth-cell and stop-cell observations. The byte-level parser in the ESP firmware is
unambiguous only for HEAT 18–23 °C; setpoints above 23 °C and below 18 °C in HEAT need capturing, because
the missing setpoint bits fall in cells that the UART reports only as parity/framing information.

Checklist status: steps 1 (setpoint sweep, HEAT 18–23 only), 2 (fan and mode sweep, plus ECO) and 5 (idle
soak, unit off only) are captured; step 3 (single versus held press) is dropped because the remote has no
long press, and the `13` repeat is just the second round of every change; step 4 is done; step 6 is low
priority. Still to capture, in priority order: HEAT 16–17 and 24–30, the full COOL/AUTO/FAN setpoint
ranges, DRY mode, and a running-state idle soak.

## Related projects and what they suggest (1 Nov 2026)

No public project was found that documents this bus (searches for the models, remote part number and
protocol terms returned only this repository). What exists is related, not a decoder:
- `jaroslawprzybylowicz/fuji-iot` and its port `unreality/FujiHeatPump` (the vendored engine): the
  modern 3-wire family (UTY-RNNUM and similar), 8-byte frames with source, destination, register type and
  a 5-byte payload. Its field set matches what this remote sends, so it names the same features:
  enabled, mode, fan, setpoint, economy (plus swing, which this ducted unit lacks). The encodings differ:
  its mode values are FAN 1, DRY 2, COOL 3, HEAT 4, AUTO 5 and fan AUTO 0, LOW 1, MEDIUM 2, HIGH 3, setpoint
  as plain degrees in 7 bits; this bus uses COOL 1, HEAT 2, FAN 4, AUTO 5, fan HIGH 1, MEDIUM 2, LOW 4,
  AUTO 5 and a 5-bit setpoint (16–31), in a bit-serial packet that is also not 8 bytes. So this remote
  speaks an older or different variant, and the engine's frame and login logic does not apply.
- `unreality/FujiHeatPump` issue 8 ("2 wire") reports a ducted unit whose board offers either an
  older 3-wire LIN-style remote or a newer 2-wire (UTY-RLRY) remote, selected by a jumper. That is a
  third-party report, not verified here, but it fits the picture of an older remote generation on ducted
  indoor units, and it points the same way as the capture: nothing existing decodes it.
- The 2-wire/Homebus projects (Daikin P1P2, Toshiba AB) show that a fixed bit layout with no checksum
  is plausible for this kind of bus; they are not otherwise applicable.

What the captures say about complexity: the remote's whole state is five fields (power, mode, fan,
setpoint, ECO), the setting packet has no varying cell outside them across 149 packets, and the exchange
is a plain poll/reply with a doubled setting round. The encoder in `tools/fuji_packet.py` already
regenerates every observed packet exactly. What remains unknown is on the transmit side: whether the unit
accepts a setting from a second sender, and when it is safe to send. A cautious order, none of it done:
1. Bench: drive the encoder's pulse list from an ESP32 GPIO (timed, 1980.8 µs per cell, open-drain through the
   same LIN transceiver) with the remote disconnected, and compare the result cell for cell against a
   captured packet on the logic analyser.
2. Live, remote connected: send the setting the remote already holds (a no-change write) right after a unit
   reply, in the 30–57 ms before the next poll, and see whether the unit echoes it and the remote display
   stays consistent.
3. Only then change one field at a time, keeping the control-enabled switch and a physical disconnect.
The unit stalling for a second and replying with `04`, and the one `07` reply byte, show that the unit
sometimes behaves unexpectedly while running, so step 2 should be watched on the analyser.

## Labeled ESP logs: setpoint ranges (1 Nov 2026, evening)

New labeled ESP logs (`my-test`, `seq`, `eco`, `start-test`, `idle`) cover the full setpoint ranges.
`tests/esp_log_model_test.py` shows that the cell-layout encoder predicts every setting byte seen in them:
HEAT 16–30, COOL 18–30 and AUTO 18–30, one degree per press, with COOL and AUTO clamping at 30, and ECO as
a flag in the second byte in every mode. Details and the byte tables are in
[`../captures/matt-captures/settings-captures-notes.md`](../captures/matt-captures/settings-captures-notes.md).
Operator labels were used only to locate the sweeps.

Consequence for the firmware: the ESP's byte-level view cannot tell HEAT 24–30 from 16–22 (and a few bytes are
shared between modes), so HA currently shows an aliased setpoint for HEAT above 23 °C and ignores unmapped
COOL/AUTO values. The fix is a cell-level decoder on the ESP32 using GPIO edge timestamps, not a bigger byte
table. Remote ranges, confirmed by the operator: HEAT 16–30, COOL 18–30, AUTO 18–30, FAN no setpoint. The captured COOL and
AUTO sweeps started at 18, so the lower limits of those two modes rest on the operator, not on the bus.
Remaining capture needs are small: a logic-analyser confirmation of the extreme setpoints, and a
running-state idle soak.

## Wired-remote emulator firmware (1 Nov 2026, experimental, not yet run on hardware)

**Why an emulator.** On this bus the wired remote is the master: it polls every ~247 ms and the unit only
replies. A setting message is 164 ms long, but the free gap between the unit's reply and the next poll is only
30–58 ms, so nothing can be injected while a real remote is connected (the remote does not wait for a free
slot). The only way to send a command is to take the remote's place: **disconnect the wired remote**, and let
the ESP32 poll and send settings exactly as the captures show.

**What was added** (updated 1 Nov 2026, evening: simplified to one button, no switches):
- `FujiAot30Cells.h` builds the 84-cell setting, 44-cell poll and 33-cell off messages (mirrors
  `tools/fuji_packet.py`; checked against 677 generated vectors in `tests/golden_cells.txt`).
- `FujiAot30Scheduler.h` is the timing script from the captures: a change goes out in place of a poll
  (round 1), a poll flagged `0A` 405 ms later, round 2 at 803 ms, another `0A` poll at 1210 ms, then normal polls
  (run flag set) every 247 ms. Power-off is the off message, a poll still flagged running at +167 ms, then idle polls
  from +405 ms (not used by the button).
- One button, **"AOT30 Test: HEAT 20 ON (remote disconnected)"**, runs a fixed test of about 20 s with no other
  controls: it waits for 1.5 s of bus silence, polls for 2 s (unit assumed off), sends HEAT 20 °C, auto fan, ECO off
  as the remote does, keeps polling for 10 s, then stops and releases the bus. While running it drives GPIO17 as a
  plain pin at 1980.8 µs per cell and reads GPIO16 back every cell; three mismatches (the line not following what was
  driven, or another device pulling it low) abort, release the line and end the test. It refuses to run if a message from
  the wired remote was seen in the 1.5 s before pressing or during the silence check, which a connected remote would
  cause. Stray bytes from the unit itself do not block it: the operator reports that with no remote connected the unit
  sends something like eight zeros or `00` about 5.2 s after power-up (not yet captured; this corrects the earlier
  statement that the unit puts no traffic at all on RX without the remote).
  The log lines to watch start with "AOT30 test" and "AOT30 emulator".
- The earlier one-shot UART test buttons, the arm switch and the dry-run/enable switches were removed. After the test
  ends the ESP32 no longer polls, so reconnect the wired remote (it takes over) or the unit will sit without a master.

**Tests** (`sh tests/run_all.sh`): the cell encoder against the Python model, the scheduler timeline (including
late sends and millis wrap), and the real `FujiHeatPump.cpp` compiled on a desktop with a fake clock, UART and
GPIO: dry run, refusal when the bus is busy, the pulse train timing within 10 µs of the encoded cells, a full
change and power-off, abort on read-back mismatch, and disabling. It could not be compiled for the ESP32 here
(the ESPHome package is not available from the internal package index), so the ESP-IDF calls
(`esp_rom_gpio_connect_out_signal`, `gpio_*`, `uart_set_pin`, `esp_timer_get_time`) are unverified until built.

**Suggested run, stopping at the first surprise:**
1. Build and flash. Nothing happens until the button is pressed.
2. Bench first if you can: unit disconnected, wired remote disconnected, logic analyser on the ESP32 line. Press
   the button and decode the capture with `tools/decode_logic_capture.py --settings` (it should show HEAT 20 °C, auto
   fan, both rounds); the bit time should read 1980.8 µs.
3. Live: unit powered (off or on), wired remote disconnected, press the button. On the analyser look for the unit's
   replies `7F 18 13 48 80` (and `7F 12 01` as its first message after power-up), then its echoes `7F 12 03 …`
   after the setting. Reconnect the wired remote when the log says the test has finished.

**Known risks, none tested:**
- The LIN transceiver's TXD dominant timeout (typically 6–20 ms) may cut off the remote's first byte, whose low
  pulse is 15.8 ms long. The read-back check would show this as an abort at cell 0–7; if it happens the transmitter
  hardware, not the protocol, is the limit.
- The first poll goes out with the run flag off (`13`) because nothing has been heard from the unit yet; after its first
  reply the poll flag follows the unit's, as the real remote's does (1,592 of 1,600 captured polls repeated the unit's
  last reply flag; the exceptions are the polls right after the remote itself sent a change). A real remote that is
  power-cycled while the unit runs rejoins, which suggests the flag is informational, but a capture of that rejoin
  (unit running, remote's power removed and restored) would show exactly what the remote sends first.
- If the real remote is reconnected while the emulator runs, the two collide. The read-back abort limits it, but the
  unit may already have seen a corrupted message. Reconnect the remote only after the test has finished.
- Whether the unit accepts commands from the ESP32 is unknown. A failure to respond may also mean the exchange needs
  something the captures never showed (for example a different start-up sequence); capture it on the analyser and
  compare with `start-up.sr`.
