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
