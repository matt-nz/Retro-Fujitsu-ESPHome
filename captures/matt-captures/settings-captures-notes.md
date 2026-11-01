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

- Setpoints 16, 17 and 24–30 in HEAT and the full COOL and AUTO ranges (FAN carries no setpoint). The
  installation is ducted with no DRY mode and no vanes, so the remote's whole feature set is power,
  the modes AUTO/COOL/HEAT/FAN, four fan speeds plus auto, setpoint and ECO, and all of them are now
  mapped except the untested setpoint range.
- What makes the repeat round appear or not; whether cells 33–44 contain bits beyond those mapped.
- The unit reply deviations above. A running-state idle soak would show how often they occur.
- Whether the unit accepts a message from a second sender, and what the constant cells (including the
  first 33 and last 29 cells) are for; no checksum has been found, which fits with all non-field cells
  being constant.

## Encoder (model of the captures, does not transmit)

[`tools/fuji_packet.py`](../../tools/fuji_packet.py) builds the 84-cell packet from (mode, fan, setpoint,
ECO, round, sender) and the pulse list at the sender's bit time, e.g.
`python3 tools/fuji_packet.py heat 22 auto`. `tests/logic_capture_test.py` checks that it reproduces all
149 captured packets cell for cell, so the field layout above is sufficient to regenerate every observed
packet. It also predicts untested values (for example HEAT 24 °C sets cell 43); those predictions are
not verified.
