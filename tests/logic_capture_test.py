#!/usr/bin/env python3
"""Checks tools/decode_logic_capture.py against the committed logic-analyser captures.

Run from the repository root:  python3 tests/logic_capture_test.py
Standard library only. Exits non-zero on the first failed expectation.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import decode_logic_capture as d  # noqa: E402

CAPTURES = os.path.join(os.path.dirname(__file__), "..", "captures", "matt-captures")
failures = []


def expect(name, ok, got=None):
    if not ok:
        failures.append("%s%s" % (name, "" if got is None else ": got %r" % (got,)))


def load(name):
    starts, levels, total = d.load_samples(os.path.join(CAPTURES, name + ".sr"), 1)
    frames = d.annotate(d.decode_frames(starts, levels, total))
    clocks = d.sender_clocks(frames)
    return starts, levels, frames, clocks


def remote_settings(name, repeats=False):
    starts, levels, frames, clocks = load(name)
    out = []
    for m, cells in d.packet_cells(starts, levels, frames, clocks):
        if m[0]["sender"] != "remote":
            continue
        r = d.decode_setting_cells(cells)
        if bool(r["round"]) == repeats:
            out.append(r["setting"])
    return out


def main():
    # Bit clocks are the same in every capture.
    for name in ("start-up", "heat", "fan-mode", "eco"):
        _, _, _, clocks = load(name)
        expect("%s remote clock" % name, 1980 <= clocks["remote"] <= 1982, clocks["remote"])
        expect("%s unit clock" % name, 2052 <= clocks["unit"] <= 2060, clocks["unit"])

    # Operator-described sequences, first transmission of each change.
    heat = remote_settings("heat")
    expect("heat: setpoints 18..23..18, auto fan, eco off",
           heat == [("HEAT", t, "auto", False) for t in (18, 19, 20, 21, 22, 23, 22, 21, 20, 19, 18)], heat)
    expect("heat: every change repeated once", remote_settings("heat", repeats=True) == heat)

    fan = remote_settings("fan-mode")
    expect("fan-mode sequence", fan == [
        ("HEAT", 18, "auto", False), ("HEAT", 18, "high", False), ("HEAT", 18, "medium", False),
        ("HEAT", 18, "low", False), ("HEAT", 18, "auto", False), ("FAN", None, "auto", False),
        ("COOL", 23, "auto", False), ("AUTO", 18, "auto", False)], fan)

    eco = remote_settings("eco")
    expect("eco flags", [s[3] for s in eco] == [False, True, False, True, False], eco)
    expect("eco only changes the eco flag", len(set(s[:3] for s in eco)) == 1, eco)

    # Every cell outside the known fields is constant in every remote setting packet.
    packets = []
    for name in ("start-up", "heat", "fan-mode", "eco"):
        starts, levels, frames, clocks = load(name)
        for m, cells in d.packet_cells(starts, levels, frames, clocks):
            packets.append((m[0]["sender"], cells))
    known = set()
    for first, width in d.FIELD_CELLS.values():
        known |= set(range(first, first + width))
    for sender in ("remote", "unit"):
        sel = [c for s, c in packets if s == sender]
        varying = {i for i in range(d.PACKET_CELLS) if len({c[i] for c in sel}) > 1}
        expect("%s: cells outside the known fields never vary" % sender, varying <= known, sorted(varying - known))
    remote = [c for s, c in packets if s == "remote"]
    unit = [c for s, c in packets if s == "unit"]
    # The unit echoes the setting with only its sender nibble (cells 16 and 17) different.
    expect("echo differs from remote only in cells 16/17",
           {i for i in range(d.PACKET_CELLS) if len({c[i] for c in remote + unit}) > 1} - known == {16, 17})

    if failures:
        print("FAILED:\n  " + "\n  ".join(failures))
        sys.exit(1)
    print("ALL PASSED (%d setting packets)" % len(packets))


if __name__ == "__main__":
    main()
