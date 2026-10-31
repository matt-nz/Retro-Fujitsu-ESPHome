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
