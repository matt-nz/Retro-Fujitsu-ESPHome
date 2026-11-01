# Startup bus capture

## Capture context

- Raw sigrok capture: [`start-up.sr`](start-up.sr)
- PulseView session: [`start-up.pvs`](start-up.pvs)
- Sample rate: 1 MHz; the archive contains 23,309,209 samples.
- PulseView assigns D1 to RX. Its saved UART decoder is set to 500 baud and even
  parity; it does not record an explicit data-bit or stop-bit setting.
- The operator reports turning the unit on, raising the setpoint several times, then
  turning it off. Individual button-press times were not marked in the capture.

## Preliminary raw-signal observations

Sampling D1 at approximately 1.98 ms per bit gives repeated 500-baud-looking words.
The following notation shows the ninth sampled bit before the eight-bit value; for
example, `1EC` means ninth bit 1 followed by byte `EC`. Inverting each byte (XOR `FF`)
gives the bus messages already seen in the ESP logs:

| Message | Captured 9-bit words | Inverted byte stream |
|---|---|---|
| Off-state poll | `080 0D7 1EC 1F7` | `7F 28 13 08` |
| On-state poll | `080 0D7 1E8 1F7` | `7F 28 17 08` |
| On-state reply | `080 0E7 1E8 1B7 07F` | `7F 18 17 48 80` |
| Setting change | `080 0DD 1FC 129 0FD 1A0 081` | `7F 22 03 D6 02 5F 7E` |
| Power-off | `080 0DB 1FC` | `7F 24 03` |

The ninth-bit pattern is stable for the repeated polls (`0011`) and replies (`00110`).
The setting message above has pattern `0011010`. This is not consistent with ordinary
even parity: for example, the raw `80` byte has one set data bit but ninth bit 0, and
the raw `B7` byte has six set bits but ninth bit 1. These examples rule out treating
the ninth bit as a reliable even-parity bit, assuming the sampled byte alignment is
correct.

This supports investigating a ninth data/marker bit, but does not establish standard
9N1 framing. A preliminary fixed-cell decoder also sampled a low level in the expected
stop-bit cell for some setting-transaction bytes. Those frames need careful timing and
frame-boundary validation before claiming a UART format. The nine-bit PulseView view
removing parity annotations is not, by itself, evidence that the stop bits are valid.

These are preliminary observations from one capture, not a protocol specification.
Do not use them to change the ESP UART configuration or synthesize transmissions.

## Validated decode of `start-up.sr` (1 Nov 2026)

Reproduce with `python3 tools/decode_logic_capture.py captures/matt-captures/start-up.sr
--check --messages` (standard library only; `--check` asserts every number below).

**Capture facts.** Only D1 changes (D0 and D2–D7 stay high, so the ESP32 TX line is idle). The line
idles high; it reads low for the first 0.29 s of the file, then high. The file is 23.309 s long and
decodes to 659 complete bytes in 142 messages. A byte that would run past the end of the file is
dropped.

**Two transmitters with different bit clocks.** Measured from falling edge to falling edge of plain
11-cell bytes (the accurate measure; low pulses read about 12 µs wide because of the slow rising edge):

| Sender | Messages it starts | Bit time | Baud | Spread |
|---|---|---|---|---|
| Wired remote | `7F 28 ..` poll, `7F 22 ..` setting, `7F 24 ..` off | **1980.8 µs** | 504.8 | 21786–21792 µs per byte (±0.03 %) |
| Indoor unit | `7F 18 ..` reply, `7F 12 ..` echo, `7F 14 ..` off echo, `7F 12 01` | **2055.6 µs** | 486.5 | 22561–22761 µs per byte (±0.5 %) |

Neither is 500 baud: the remote is 0.96 % fast, the unit 2.78 % slow, and the two differ from each
other by 3.8 %. The remote's clock is very steady, the unit's is looser. The high nibble of the
second message byte therefore identifies the sender: `2` = remote, `1` = unit (all 142 messages agree: 71 each).

**Frame shape: eleven cells, not standard 8E1.** Each byte is start (0), eight data cells LSB first,
a ninth cell, then the stop cell, with the next start edge normally on cell 11. Data are the inverted
bus bytes, as in the ESP logs.
- **The ninth cell is not parity.** It differs from even parity of the data in 392 of 659 bytes. It is
  fixed by the byte's position in the message, independent of the data value and of the sender:
  positions 0–6 give `0 0 1 1 0 1 0` (every setting message, both senders, including all values of the
  setpoint byte; polls and replies show the same prefix `0 0 1 1 (0)`). Examples: `7F` (one bit set)
  carries 0 where even parity wants 1; `B7` (six bits) carries 1 where it wants 0. The one exception in
  the file (`7F 18 17`, then `04` at 15.187 s) is the stalled reply described below. Position-only
  behaviour means it gives no data protection; the message must be validated some other way, and no
  checksum has been found.
- **Stop cell low in 22 bytes, all in setting messages:** bytes 2 and 3 (`03`/`13` and the value) of each
  of the 11 setting messages, from both senders (the unit's echo has the same shape). The low cell
  lasts exactly one cell of the sender's own bit time (1992/1995 µs for the remote, 2045–2059 µs for the
  unit, with the same rise-edge offset), so it is part of the sending device's waveform, not a collision
  with the other device. It is followed by one high cell and then the next start edge, so the byte
  occupies 12 cells (byte 2) or 13 cells (byte 3). Byte 5 (`5F`) is followed by 14 cells in total before the
  next start edge or reply.
- Byte-to-byte spans (cells) per message type:

  | Message | Spans between byte starts |
  |---|---|
  | poll `7F 28 1x 08/0A` | 11 11 11 |
  | reply `7F 18 1x 48 80` | 11 11 11 13 |
  | setting `7F 22/12 0x|1x vv 02 5F 7E` | 11 11 12 13 11 14 |
  | off `7F 24 03` / echo `7F 14 03` | 11 11 |
  | start-up `7F 12 01` | 11 11 |

**What a fixed 500 baud 8E1 receiver (the ESP32 UART) sees.** Replaying the samples through such a
receiver (start edge, mid-cell sampling at 2000 µs) returns the same 659 bytes, flagging 392 parity
errors and 22 framing errors. So the data bytes are delivered intact (as in the ESP logs) only because
the receiver passes bytes through despite those errors. Margin is small for the unit: its stop cell
spans 20.56–22.61 ms from its start edge and a 500 baud receiver samples it at 21.0 ms, 0.2 of a cell
from the cell's leading edge; another few percent of clock error would misread it.

**Message timing (this capture).**
- Poll start to next poll start: median 246.6 ms (237.9–388.9, n = 60).
- Poll end to reply start: median 5.9 ms (1.7–10.0, n = 62). Reply end to the next poll: median 36.4 ms
  (29.8–57.8).
- A setting message is followed by the unit's echo 22–28 ms after it ends, then a remote `7F 28 17 0A`
  (42–49 ms later), then the unit's echo a second time (4–8 ms later). Power-off: `24 03`, then
  `14 03` 8 ms later.

**Events in this capture (decoded, bus bytes).**

| Time (s) | Message | Decoded |
|---|---|---|
| 3.599 | first poll `7F 28 13 08` | unit powers up |
| 3.690 | unit `7F 12 01` | sent in place of the normal reply to the first poll |
| 4.940 | remote `7F 22 03 D6 02 5F 7E`, then echoes | HEAT 22°C, fan AUTO (by the bit-field model) |
| 5.348 | remote `7F 28 17 0A`, polls become `28 17` | unit running |
| 14.130–15.187 | unit reply `7F 18 17` stalls for 1.01 s, then a lone byte `04` | see below |
| 16.894 | `7F 22 03 B6 ...`; repeated at 17.696 as `22 13 B6` | HEAT 21°C |
| 19.065 | `7F 22 03 96 ...` | HEAT 20°C |
| 19.473 | `7F 24 03`, echo `7F 14 03` at 19.547 | off |

The operator reports starting at HEAT 20 and raising the setpoint three times; the decoded values go
22 → 21 → 20, a fall of two, and there are only three setting messages (plus the `13` repeat). Either
the report is loose or the file is not the run described; the bit-field model decodes `96`, `B6`, `D6`,
`F6` as 20, 21, 22, 23 with AUTO fan.

**Unexplained.**
- The unit's reply at 14.130 s stops after three bytes (`7F 18 17`), the bus is silent for 1.01 s, a single
  `04` follows (instead of `48 80`), then normal polling resumes. No remote message was sent in that
  interval, so a collision with the remote is not supported by the data. `04` may be a status or error
  value.
- What the extra low cell and extra high cells in setting messages mean, and why the ninth cell follows a
  fixed per-position sequence. Both look like properties of the sender's frame generator.
- The meaning of `03` versus `13`, and of `08` versus `0A`, in the polls and settings.

**Consequences for transmitting.** A UART configured as 500 baud 8E1 cannot reproduce these bytes: the
remote sends at 504.8 baud, a ninth cell that is not parity, and a low stop cell in two bytes of every
setting message. Any transmit experiment would need cell-level control of the line (for example bit-banged
or RMT timing at 1980.8 µs per cell) and should copy the remote's exact cell pattern. The ninth cell could
be forced with the hardware parity setting by choosing even or odd parity per byte, but the low stop cell
and the extra cells cannot. Nothing here shows the unit accepts a message from a second sender.

## Update (1 Nov 2026, later): the odd cells are packet fields

The ninth-cell sequence and the 22 low stop cells above are explained by the later captures: after the
second byte, a setting message is a bit-serial packet with fixed field positions, and cutting it at falling
edges mis-frames it. See [`settings-captures-notes.md`](settings-captures-notes.md). The bit clocks, 11-cell
frames for the first two bytes, message timing and the unexplained stalled reply remain as above.
