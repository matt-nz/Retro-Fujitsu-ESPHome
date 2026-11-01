#!/usr/bin/env python3
"""Checks the packet model against the labeled ESP logs (UART-level bytes).

Run from the repository root:  python3 tests/esp_log_model_test.py

The ESP logs only carry the bytes a fixed 500 baud 8E1 UART delivers. The test turns every setting the
encoder (tools/fuji_packet.py) can build into its pulse train, runs it through the same fixed-rate
receiver model used for the logic captures, and requires that (1) every setting message seen in the
labeled logs is one the model produces and (2) the remote's setpoint sweeps appear in the logs in the
order the model predicts. Operator labels were only used to choose which sweep to look at; they are
not relied on for the pass/fail result.
"""

import os
import sys

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import decode_logic_capture as d  # noqa: E402
import decode_timeline as dt  # noqa: E402
import fuji_packet as fp  # noqa: E402

CAPTURES = os.path.join(HERE, "..", "captures", "matt-captures")
failures = []


def expect(name, ok, got=None):
    if not ok:
        failures.append("%s%s" % (name, "" if got is None else ": got %r" % (got,)))


def uart_view(cells):
    """Bus bytes a fixed 500 baud 8E1 receiver reports for one encoded packet."""
    starts, levels, t = [0.0], [1], 10000.0
    for level, us in fp.runs(cells):
        starts.append(t)
        levels.append(level)
        t += us
    starts.append(t)
    levels.append(1)
    sim = d.simulate_nominal_uart(starts, levels, t + 30000.0)
    return [v ^ 0xFF for _, v, _, _ in sim]


def model_table():
    table = {}
    for mode in ("heat", "cool", "auto", "fan"):
        low, high = fp.TEMP_RANGES.get(mode, (None, None))
        temps = [None] if mode == "fan" else range(low, high + 1)
        for fan in fp.FANS:
            for temp in temps:
                for eco in (False, True):
                    b = uart_view(fp.encode(mode, fan, temp, eco))
                    expect("model packet decodes to 7 bytes for %s %s %s" % (mode, fan, temp), len(b) >= 7, b)
                    table[(mode, fan, temp, eco)] = (b[3], b[4])
    return table


def remote_settings(name):
    """Remote first-round setting messages (value byte, flag byte) in log order."""
    out = []
    with open(os.path.join(CAPTURES, name + ".log"), errors="replace") as f:
        for kind, _, data in dt.messages(f):
            b = list(data) if kind == "msg" else []
            if len(b) == 7 and b[0] == 0x7F and b[1] == 0x22 and b[2] == 0x03:
                out.append((b[3], b[4]))
    return out


def squeeze(values):
    out = []
    for v in values:
        if not out or out[-1] != v:
            out.append(v)
    return out


def main():
    table = model_table()
    produced = {v for v in table.values()}

    for name in ("my-test", "seq", "eco", "cold-test"):
        seen = set(remote_settings(name))
        missing = sorted(seen - produced)
        expect("%s: every observed setting is produced by the model" % name, not missing,
               ["%02X %02X" % m for m in missing])

    # Sweeps, in the order they appear in the logs (adjacent repeats removed).
    seq = [v for v, _ in remote_settings("seq")]
    heat_16_30 = [fp_value for fp_value in (table[("heat", "auto", t, False)][0] for t in range(16, 31))]
    expect("seq.log contains the HEAT 16..30 sweep as one run", _contains(squeeze(seq), heat_16_30), None)
    auto_18_30 = [table[("auto", "auto", t, False)][0] for t in range(18, 31)]
    expect("seq.log contains the AUTO 18..30 sweep", _contains(squeeze(seq), auto_18_30), None)
    cool = [v for v, _ in remote_settings("cold-test")]
    cool_18_30 = [table[("cool", "auto", t, False)][0] for t in range(18, 31)]
    expect("cold-test.log contains the COOL 18..30 sweep", _contains(squeeze(cool), cool_18_30), None)

    # COOL and AUTO clamp at 30: the extra presses repeat the last value (cold-test T31, my-test T30+).
    expect("COOL stays at 30 on the press past it", cool.count(table[("cool", "auto", 30, False)][0]) >= 2)
    my = [v for v, _ in remote_settings("my-test")]
    expect("AUTO stays at 30 on later presses", my.count(table[("auto", "auto", 30, False)][0]) >= 2)

    # Fan sweep at AUTO 18 and HEAT 20, and ECO, as the model predicts them.
    ecolog = remote_settings("eco")
    expect("AUTO 18 fan high/medium/low", [table[("auto", f, 18, False)][0] for f in ("high", "medium", "low")] ==
           [0x23, 0x25, 0x29])
    expect("fan values appear in eco.log", all(v in {x for x, _ in ecolog} for v in (0x23, 0x25, 0x29, 0x86, 0x8A, 0x92)))
    expect("ECO sets the flag byte to 42 only", {b for _, b in ecolog} == {0x02, 0x42})

    # Known aliasing in the byte view: HEAT 16/24, 17/25, ..., 22/30 give identical bytes.
    for t in range(16, 23):
        expect("HEAT %d and %d alias in the byte view" % (t, t + 8),
               table[("heat", "auto", t, False)] == table[("heat", "auto", t + 8, False)])

    if failures:
        print("FAILED:\n  " + "\n  ".join(failures))
        sys.exit(1)
    print("ALL PASSED (%d modelled settings, %d distinct byte pairs)" % (len(table), len(produced)))


def _contains(haystack, needle):
    n = len(needle)
    return any(haystack[i:i + n] == needle for i in range(len(haystack) - n + 1))


if __name__ == "__main__":
    main()
