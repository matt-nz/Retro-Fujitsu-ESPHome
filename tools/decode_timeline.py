#!/usr/bin/env python3
"""Print a timeline of bus messages from an ESPHome capture log, grouped by your labels.

Usage:  python3 tools/decode_timeline.py captures/matt-captures/my-test.log [--all]

Reads the "RX raw" lines, inverts each byte (XOR 0xFF), splits the stream at 0x7F and prints
every message except the routine poll/reply pairs (use --all to include those). Lines marked
[LABEL] (written by tools/capture_labels.py) start a new group. A gap of more than one second
between log lines is shown as a discontinuity, since concatenated sessions are not contiguous.

This only shows raw values; it does not interpret them. The values mapped to mode, fan and
setpoint are in components/fujitsu_climate/FujiPassiveStream.h.
"""

import re
import sys

LINE = re.compile(r"\[(\d+):(\d+):(\d+\.\d+)\]")
RAW = re.compile(r"RX raw \(\d+/8, before XOR\):((?: [0-9A-Fa-f]{2})+)")
LABEL = re.compile(r"\[LABEL\]\s*(.*)")

ROUTINE = {
    (0x28, 0x13, 0x08),
    (0x28, 0x17, 0x08),
    (0x18, 0x13, 0x48, 0x80),
    (0x18, 0x17, 0x48, 0x80),
}


def seconds(match):
    h, m, s = match.groups()
    return int(h) * 3600 + int(m) * 60 + float(s)


def clock(t):
    return "%02d:%02d:%06.3f" % (t // 3600, (t % 3600) // 60, t % 60)


def messages(lines):
    """Yield ('label', t, text), ('gap', t, None) or ('msg', t, bytes) in log order."""
    cur, cur_t, prev_t = [], None, None
    for line in lines:
        stamp = LINE.search(line)
        if not stamp:
            continue
        t = seconds(stamp)
        label = LABEL.search(line)
        if label:
            yield ("label", t, label.group(1))
            continue
        raw = RAW.search(line)
        if not raw:
            continue
        if prev_t is not None and t - prev_t > 1.0:
            if cur:
                yield ("msg", cur_t, bytes(cur))
            cur = []
            yield ("gap", t, None)
        prev_t = t
        for token in raw.group(1).split():
            b = int(token, 16) ^ 0xFF
            if b == 0x7F:
                if cur:
                    yield ("msg", cur_t, bytes(cur))
                cur, cur_t = [], t
            if cur or b == 0x7F:
                cur.append(b)
    if cur:
        yield ("msg", cur_t, bytes(cur))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv
    if len(args) != 1:
        sys.exit(__doc__)
    with open(args[0], errors="replace") as f:
        t0 = None
        last_running = None
        for kind, t, data in messages(f):
            if t0 is None:
                t0 = t
            if kind == "label":
                print("\n== %s  %s  (+%.1fs)" % (clock(t), data, t - t0))
            elif kind == "gap":
                print("-- gap in log --")
                last_running = None
            else:
                body = tuple(data[1:])
                text = " ".join("%02X" % b for b in data)
                if body in ROUTINE:
                    running = body[1] == 0x17
                    if last_running is not None and running != last_running:
                        print("%s  unit %s  (%s)" % (clock(t), "RUNNING" if running else "IDLE", text))
                    last_running = running
                    if not show_all:
                        continue
                print("%s  %s" % (clock(t), text))


if __name__ == "__main__":
    main()
