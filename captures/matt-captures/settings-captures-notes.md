# Setting packets decoded by cell position (heat, fan-mode, eco, idle captures; 1 Nov 2026)

Captures: [`heat.sr`](heat.sr) (HEAT setpoint 18→23→18, fan auto), [`fan-mode.sr`](fan-mode.sr)
(HEAT 18 fan sweep, then FAN, COOL, AUTO modes), [`eco.sr`](eco.sr) (ECO toggled on/off twice),
[`idle.sr`](idle.sr) (281 s, unit off, no presses) and the earlier [`start-up.sr`](start-up.sr). All
D1 at 1 MHz. Reproduce with `python3 tools/decode_logic_capture.py <file>.sr --settings` and
`python3 tests/logic_capture_test.py`.

## Result: a setting message is a fixed bit layout, not UART bytes

The first two bytes of a message (`7F`, then `22` from the remote or `12` from the unit) are ordinary
11-cell UART-style bytes. From the third byte on, the fields sit at **fixed cell positions** counted
from the message's first falling edge (cell width = the sender's bit time: 1980.8 µs remote, 2055.6 µs
unit). Comparing all 149 setting packets in the four captures that contain them (50 from the remote, 99 from
the unit; `idle.sr` has none),
exactly these cells ever vary (line level inverted gives the bus bit; fields are least significant bit
first):

| Cells | Field | Values seen |
|---|---|---|
| 16–17 | sender (`2` remote / `1` unit in the second byte's high nibble) | remote vs unit only |
| 27 | round: `0` first transmission (`03`), `1` repeat (`13`) | 0, 1 |
| 33–35 | mode | 1 COOL, 2 HEAT, 4 FAN, 5 AUTO (DRY not captured) |
| 37–39 | fan | 1 high, 2 medium, 4 low, 5 auto |
| 40–44 | setpoint °C (5 bits, 16–31) | 18–23 in HEAT, 23 COOL, 18 AUTO, FAN reads 16 |
| 54 | ECO | 0 off, 1 on |

Every other cell of the 84-cell packet is identical in all of them. Cell 36 is constant `0`; the
setpoint's bits 3 and 4 (cells 43–44) read `0` and `1` for every captured setpoint because 16–23 all
have those values. The unit's echo is cell-for-cell the same as the remote's message apart from
cells 16–17, so the unit repeats the settings it was sent.

The decode reproduces every operator-described sequence exactly: HEAT 18, 19, … 23, 22, … 18; fan
auto → high → medium → low → auto; HEAT → FAN → COOL (23) → AUTO (18); ECO off/on/off/on/off with
nothing else changing. It is checked by `tests/logic_capture_test.py`.

## This explains the earlier UART-level oddities

The "bytes" after the second were an artefact of cutting a bit-serial packet at falling edges:
- The apparent value byte (`56`, `76`, `BD`, …) is 8 cells starting at the next falling edge, whose position
  depends on the mode bits (HEAT starts 2 cells before COOL, AUTO/FAN 1 cell before), so the byte
  mixes mode, fan and setpoint bits differently per mode. That is the earlier "bit-field model" (shift
  0/1/2 by mode); the real layout above supersedes it.
- The "ninth cell" and the "stop cell low" of the value byte are the setpoint's bits 3 and 4 (cells 43
  and 44) for HEAT, so they are packet content, not parity or a framing violation. Pos-2/pos-3 byte
  lengths of 12/13 cells, and the variants 13/12 and 14/11 for FAN/AUTO/COOL, are the same effect
  (the pair always adds up to 25 cells).
- Consequence for the ESP32 byte decoder: in HEAT the value byte contains only the low 3 bits of the
  setpoint, so 16 and 24 °C (and 17/25 … 22/30) give the same byte; the missing bits arrive in cells
  the UART reports as parity/framing information. The current byte table is only unambiguous for the
  captured 18–23 °C HEAT range. Setpoints above 23 °C and below 18 °C have not been captured.

## Exchange pattern and timing

A change is sent twice, 0.80 s apart (`22 03 …` then `22 13 …`). Each round is: setting from the
remote in place of a poll, the unit's echo ~190 ms later, a remote poll `7F 28 17 0A` ~215 ms after
that, then the unit's echo again ~90 ms later. Then normal `28 17 08` polling resumes. Repeats were
missing three times in 50 transmissions: the first setting after power-up (`start-up.sr`), the
switch to FAN mode, and the last ECO change; a fourth (`96` before power-off) was cut short by the
off message. Reason unknown. Power-off is `24 03` / echo `14 03` ~78 ms later and is not repeated.

Idle soak (`idle.sr`, 281.4 s, unit off): 1155 polls `7F 28 13 08` and 1155 replies `7F 18 13 48 80`,
no other message, no `0A` poll, no stall, no byte variation. Poll period 238.0–249.4 ms (median
246.6, sd 5.0); poll end to reply start 1.75–6.09 ms (median 3.86); the remote's byte span is 21788 µs
in both the first and last quarter, so its clock is steady over minutes. This soak covers the off
state only, so it does not test the running state. It shows no room temperature or other value on the
bus while the unit is off (the same holds for earlier warm-day logs, see the status note).

## Unit reply byte 2 is not always 13/17

`heat.sr` has one unit reply `7F 18 07 48 80` at 26.966 s (unit running), between replies `7F 18 17 …`
at 26.719 and 27.214 s: byte 2 lost bit 4 (`17` → `07`) for one reply, about 2 s after the last setpoint
step. With the stalled reply followed by `04` in `start-up.sr`, these are the only deviations from
`48 80` / `13` / `17` in about 1,550 replies across the five captures, and both occur while the unit is
running. Cause unknown; worth watching for a running-state idle soak.

## Open items

- The lower limits of COOL and AUTO (18, per the operator) were never pressed down to on the bus, since both
  sweeps started at 18. Logic-analyser confirmation of the setpoint ranges above (HEAT 16–17 and 24–30, COOL and AUTO 18–30):
  they are established from the ESP logs only, at byte level. The installation is ducted with no DRY mode
  and no vanes, so the remote's whole feature set is power, the modes AUTO/COOL/HEAT/FAN, four fan
  speeds plus auto, setpoint and ECO, and all of them are now mapped.
- What makes the repeat round appear or not; whether cells 33–44 contain bits beyond those mapped.
- The unit reply deviations above. A running-state idle soak would show how often they occur.
- Whether the unit accepts a message from a second sender, and what the constant cells (including the
  first 33 and last 29 cells) are for; no checksum has been found, which fits with all non-field cells
  being constant.

## Setpoint ranges from the labeled ESP logs (`my-test`, `seq`, `eco`, `cold-test`, `start-test`, `idle`)

These ESP logs carry only UART-level bytes, with the operator's `[LABEL]` lines. The labels are
sometimes before and sometimes after the change, or wrong, so they were used only to pick out the
sweeps; the conclusions rest on the byte sequences and are checked by `tests/esp_log_model_test.py`.
The test builds every setting with the encoder, passes it through the fixed 500 baud 8E1 receiver
model, and requires that:
- all 43 distinct (value, flag) pairs seen in the four labeled sweep logs are produced by the model, so
  the cell layout predicts bytes in ranges that were never on the logic analyser;
- the logs contain the sweeps in the predicted order: HEAT 16→30, AUTO 18→30 and COOL 18→30, each
  one degree per press;
- COOL and AUTO stay at 30 when pressed past it (the final value repeats, `F5` and `EB`); HEAT's upper
  limit was not pressed past, so 30 there rests on the label count (15 steps from 16);
- ECO sets the second byte to `42` in COOL, AUTO and HEAT alike, and the fan values at AUTO 18 and HEAT 20
  match the model.

| Mode | Setpoint range | Byte view for the whole sweep (value byte, auto fan) |
|---|---|---|
| HEAT | 16–30 (operator-confirmed) | `16 36 56 76 96 B6 D6 F6` for 16–23, then the same eight again for 24–30 (23's partner is 31) |
| COOL | 18–30 (operator-confirmed) | `95 9D A5 AD B5 BD C5 CD D5 DD E5 ED F5` (+8 per degree) |
| AUTO | 18–30 (operator-confirmed) | `2B 3B 4B 5B 6B 7B 8B 9B AB BB CB DB EB` (+0x10 per degree) |
| FAN | none | `0B` (auto fan), `03`/`05`/`09` for the other speeds |

Two limits of the byte view follow, both consequences of the UART cutting the packet at falling edges:
- **HEAT repeats:** in HEAT the value byte keeps only the setpoint's low three bits, so 24–30 °C produce the same bytes as
  16–22 °C. Only the cell positions (setpoint bits 3 and 4, cells 43–44) tell them apart.
- **Cross-mode repeats:** some bytes occur in more than one mode, for example `A5` is COOL 20 °C or
  AUTO 26 °C at medium fan, `95` COOL 18 or AUTO 25 at medium. The remote's own state tells these apart in practice, the byte alone does not.

The firmware's byte table (`FujiPassiveStream.h`) only maps values from the first captures, so for
HEAT setpoints above 23 °C it shows the aliased lower setpoint (for example 26 °C appears as 18 °C), and
the COOL and AUTO values that were never mapped are ignored. Fixing this properly needs the cell positions
rather than a larger byte table: capture the line's edge times on the ESP32 (a GPIO interrupt with microsecond
timestamps is enough at 500 baud) and decode cells as `tools/decode_logic_capture.py --settings` does.

Other logs: `idle.log` is about 5.3 min (09:08:33–09:13:51) of the unit off with 5,206 replies
`7F 18 13 48 80` and 5,205 polls `7F 28 13 08`, no other message (a later stray line at 09:30 follows a
16-minute gap in the log); `start-test.log` shows the unit's first message at power-up is `7F 12 01`
(07:13:02.9) before polls `7F 28 13 08` with replies `7F 18 13 48 80`, as in `start-up.sr`.
The one unit-off soak in the logic analyser and this one agree: nothing varies while the unit is off.

## Encoder (model of the captures, does not transmit)

[`tools/fuji_packet.py`](../../tools/fuji_packet.py) builds the 84-cell packet from (mode, fan, setpoint,
ECO, round, sender) and the pulse list at the sender's bit time, e.g.
`python3 tools/fuji_packet.py heat 22 auto`. `tests/logic_capture_test.py` checks that it reproduces all
149 captured packets cell for cell, so the field layout above is sufficient to regenerate every observed
packet. It also predicts untested values (for example HEAT 24 °C sets cell 43); those predictions are
not verified.

## `test.sr`: first emulator run on hardware (2 Nov 2026, 21.2 s, 1 MHz)

Channels: D0 is the ESP32's TX (GPIO17, the transceiver's TXD input) and D1 is the bus as received (GPIO16).
The wired remote was disconnected; the ESP32 and transceiver were powered from the bus 12 V.

| Time (s) | D0 (ESP32 TX) | D1 (bus RX) |
|---|---|---|
| 0.000 – 4.2665 | high | **low** |
| 4.2665 | high | a single 62 µs high pulse, then low again |
| 4.2665 – 21.24 | high until 15.2065 | **low for the whole rest of the capture** |
| 15.2065 – 15.2302 | first poll starts: low 15.798 ms, then high 1.981, low 1.981, high 1.980, low 1.981 ms, then released high | still low |

What this shows:
- **The bit-bang transmitter works at the cell level.** The first poll's pulses are 15.798 ms (8 cells, expected
  15.846), then 1.981, 1.981, 1.980 and 1.981 ms: cell width 1980–1981 µs against the 1980.8 µs target, matching the
  encoder's pulse list. It stopped after cell 12 and released the line, exactly where the read-back check should: RX
  was low, so the expected-high cells 8, 10 and 12 were three mismatches. The safety check did its job.
- **With the remote disconnected the bus never idles high.** D1 is low from the start of the capture to the end.
  So the "bump at about 5 s" is the 62 µs *high* pulse at 4.27 s in an otherwise low line, far too short to be a
  byte (a cell is about 2 ms): a transient, not a message. There is no sign of eight zeros or `00`; the eight-zero
  idea was probably this low level.
- **Consequence:** nothing can be sent until the line idles high. In every earlier capture the remote was
  connected and the line idled high, so the remote (or something it switches in) supplies the bus pull-up, as a LIN
  master does (1 kΩ to the supply). Also, with a LIN transceiver the unit's side only pulls low; if nothing pulls the
  line up, it reads as dominant/low. Which of these holds is not known yet.
- This also explains the earlier "no traffic at all with the remote disconnected" result: a bus held low carries no
  edges. It is not evidence that the unit stays silent.

Unknowns to settle with a multimeter (unit powered, remote disconnected): the DC voltage on the LIN wire
(1) with the ESP32 module connected, and (2) with only the unit connected. About 12 V means the line does idle high
(then the low is the transceiver, e.g. its sleep/standby pin or a wake-up latch, not the bus); about 0 V with the
module connected but ~12 V without it means the module loads the bus; about 0 V either way means the unit pulls it low
or nothing pulls it up. If it is a missing pull-up, add a master-style pull-up on the ESP32 side (1 kΩ from the LIN
wire to the 12 V supply through a diode, as the LIN spec shows) and repeat the test. The firmware now refuses to start
the test, with a message saying this, when the bus is low with no traffic after the 1.5 s silence check.

### LIN wire voltages (2 Nov 2026, multimeter) and what they imply

| Condition | LIN wire | Notes |
|---|---|---|
| Wired remote connected | 8.2 – 9.3 V | recessive (idle) level with traffic, the meter averages the pulses |
| Remote disconnected, ESP32 module connected | 2.6 V | transceiver supply (VIN) at 12 V |

- 2.6 V is below the LIN dominant threshold (about 40 % of 12 V = 4.8 V), so the transceiver reads the line as
  dominant and its RX output stays low, which is what `test.sr` shows. The line is not "silent": it sits at a
  mid-rail level set by a resistor divider.
- With the remote connected the idle level is about 9 V, well above the recessive threshold (about 60 % = 7.2 V).
  So the remote contributes the pull-up to the supply, as a LIN master does; without it, the unit's side and the
  module's own 30 kΩ pull-up leave the line at 2.6 V. A rough estimate from the divider: if only the module's 30 kΩ
  pulls up, the unit side pulls down with about 8 kΩ; a pull-up of about 2 kΩ (through a diode) then gives roughly 9 V,
  which is close to what the remote gives. These are estimates from two voltages, not measurements of the resistances.
- The emulator therefore needs to supply that pull-up. Suggested try, before running the button again: a 2.2 kΩ
  resistor in series with a small diode (1N4148 or similar, anode to the 12 V supply, cathode to the resistor) from the
  12 V supply to the LIN wire at the module, fitted with a jumper or connector so it is in place only when the wired
  remote is disconnected. Check with the meter that the LIN wire then idles at about 8–10 V; if it is lower, use
  1 kΩ (the LIN master value). Fit it only while the remote is disconnected, since the remote has its own pull-up.
  After that the firmware's check should see the line high and the test button should start polling.

### Resolution (2 Nov 2026): an attached but unpowered remote clamps the line

With the wired remote disconnected completely (all three wires removed), the LIN wire at the ESP32 module sits at
12 V, so the bus idles high on its own. The 2.6 V and the permanently low RX in `test.sr` were caused by the
remote's circuitry still being attached to the LIN wire while unpowered, loading it toward ground. The pull-up
resistor suggested above is **not needed**; ignore that suggestion. For any emulator test, remove all three of the
remote's wires (at least LIN and its 12 V), not just its power.
