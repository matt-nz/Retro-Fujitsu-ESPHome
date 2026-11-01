#!/usr/bin/env python3
"""Decode a sigrok/PulseView logic capture (.sr) of the Fujitsu wired-remote bus.

Usage:
    python3 tools/decode_logic_capture.py captures/matt-captures/start-up.sr [--channel 1] [--messages] [--check]

Pure Python (standard library only). The line idles high and each byte starts with a falling
edge. For every byte the decoder:
  * measures that byte's own bit time from its edge spacing (the two transmitters on the bus use
    different bit clocks, so a fixed 500 baud grid mis-times one of them),
  * samples the eleven cells of the nominal frame (start, 8 data LSB first, ninth cell, stop),
  * records how many extra cells pass before the next start edge.
Bus bytes are the sampled data bits inverted (XOR 0xFF), matching the ESPHome logs.

It prints the transmitter bit times, frame-shape statistics, ninth-cell values by position
within a message and every stop-cell violation. --messages also prints each message with its
sender, start time and inter-message gaps. --check asserts the findings recorded in
docs/protocol-investigation-status-2026-10-31.md against the capture and exits non-zero on any
difference.
"""

import argparse
import collections
import re
import statistics
import sys
import zipfile

NOMINAL_US = 2000.0
CELLS = 11
SENDER_SPLIT_US = 2020.0  # below: wired remote, above: indoor unit


def load_samples(path, channel):
    """Return (run_starts_us, run_levels, total_us) for one channel of a 1 byte/sample capture."""
    with zipfile.ZipFile(path) as z:
        meta = z.read("metadata").decode()
        rate = re.search(r"samplerate=(\d+)\s*(k|M|G)?Hz", meta)
        scale = {"": 1, "k": 1e3, "M": 1e6, "G": 1e9}[rate.group(2) or ""]
        sample_rate = int(rate.group(1)) * scale
        unitsize = int(re.search(r"unitsize=(\d+)", meta).group(1))
        if unitsize != 1:
            sys.exit("only 1 byte/sample captures are supported")
        names = sorted((n for n in z.namelist() if re.fullmatch(r"logic-1-\d+", n)), key=lambda n: int(n.split("-")[-1]))
        table = bytes(48 + ((x >> channel) & 1) for x in range(256))
        data = b"".join(z.read(n).translate(table) for n in names)
    us_per_sample = 1e6 / sample_rate
    starts, levels = [], []
    for m in re.finditer(rb"0+|1+", data):
        starts.append(m.start() * us_per_sample)
        levels.append(1 if data[m.start()] == 49 else 0)
    return starts, levels, len(data) * us_per_sample


def level_at(starts, levels, t, hint):
    """Level at time t using a moving index so the whole decode stays linear."""
    i = hint
    while i + 1 < len(starts) and starts[i + 1] <= t:
        i += 1
    return levels[i], i


def fit_bit_time(widths):
    best = None
    t = 1900.0
    while t <= 2150.0:
        err = sum(abs(w - max(1, round(w / t)) * t) / max(1, round(w / t)) for w in widths)
        if best is None or err < best[0]:
            best = (err, t)
        t += 0.5
    return best[1]


def decode_frames(starts, levels, total_us):
    frames = []
    used_until = 0.0
    for k in range(len(starts)):
        if levels[k] != 0 or starts[k] < used_until or k == 0:
            continue
        t0 = starts[k]
        if t0 + CELLS * 2150.0 > total_us:
            break  # frame runs past the end of the capture
        edges = [t0]
        j = k + 1
        while j < len(starts) and starts[j] < t0 + 10.2 * NOMINAL_US:
            edges.append(starts[j])
            j += 1
        widths = [b - a for a, b in zip(edges, edges[1:])]
        bit = fit_bit_time(widths) if widths else NOMINAL_US
        idx = k
        cells = []
        for c in range(CELLS):
            lvl, idx = level_at(starts, levels, t0 + (c + 0.5) * bit, idx)
            cells.append(lvl)
        data = sum(cells[1 + i] << i for i in range(8))
        frames.append({"t": t0, "bit": bit, "cells": cells, "raw": data, "bus": data ^ 0xFF,
                       "ninth": cells[9], "stop": cells[10], "edges": len(edges)})
        used_until = t0 + 10.5 * bit
    return frames


def simulate_nominal_uart(starts, levels, total_us):
    """What a fixed 500 baud 8E1 receiver (such as the ESP32 UART) would deliver.

    Edge-triggered: after the stop-cell sample it waits for the next falling edge. Returns
    (time_us, raw_byte, parity_error, framing_error) per byte.
    """
    out = []
    idx = 0
    resume = 0.0
    for k in range(len(starts)):
        if levels[k] != 0 or starts[k] < resume or k == 0:
            continue  # k == 0 is the level already present when the capture began
        t0 = starts[k]
        if t0 + CELLS * 2150.0 > total_us:
            break  # same cut-off as decode_frames
        bits = []
        for c in range(CELLS):
            lvl, idx = level_at(starts, levels, t0 + (c + 0.5) * NOMINAL_US, idx)
            bits.append(lvl)
        if bits[0] != 0:
            continue  # start bit not low at its centre: glitch
        value = sum(bits[1 + i] << i for i in range(8))
        parity_err = bits[9] != bin(value).count("1") % 2
        out.append((t0, value, parity_err, bits[10] != 1))
        resume = t0 + 10.5 * NOMINAL_US
    return out


def annotate(frames):
    for f in frames:
        f["sender"] = "remote" if f["bit"] < SENDER_SPLIT_US else "unit"
    return frames


def sender_clocks(frames):
    """Bit time per sender from falling-edge to falling-edge spans of plain 11-cell bytes.

    Falling-edge spacing avoids the rise/fall threshold asymmetry of the bus (low pulses measure
    about 12 us wider than the cell), so this is the accurate clock.
    """
    clocks = {}
    for who in ("remote", "unit"):
        spans = []
        for a, b in zip(frames, frames[1:]):
            if a["sender"] == who and b["sender"] == who:
                cell = b["t"] - a["t"]
                if abs(cell / (a["bit"] * CELLS) - 1) < 0.03:
                    spans.append(cell / CELLS)
        clocks[who] = statistics.median(spans) if spans else float("nan")
    return clocks


def split_messages(frames):
    messages, cur = [], []
    for f in frames:
        if f["bus"] == 0x7F and cur and len(cur) >= 3 and f is not cur[0]:
            # 0x7F also appears as a trailing byte? It does not in the observed shapes, so it starts a message.
            messages.append(cur)
            cur = []
        cur.append(f)
    if cur:
        messages.append(cur)
    return messages


def report(frames, clocks, total_us):
    out = []
    out.append("capture length %.3f s, %d frames" % (total_us / 1e6, len(frames)))
    for who in ("remote", "unit"):
        n = sum(1 for f in frames if f["sender"] == who)
        c = clocks[who]
        out.append("  %-6s %3d frames, bit time %.1f us (%.1f baud, %+.2f%% from 2000 us)" %
                   (who, n, c, 1e6 / c, (c / NOMINAL_US - 1) * 100))
    msgs = split_messages(frames)
    out.append("messages: %d" % len(msgs))
    pos_ninth = collections.defaultdict(collections.Counter)
    for m in msgs:
        for pos, f in enumerate(m[:7]):
            pos_ninth[pos][f["ninth"]] += 1
    out.append("ninth cell by byte position within message (count of 0 / 1):")
    for pos in sorted(pos_ninth):
        c = pos_ninth[pos]
        out.append("  pos %d: 0 x%d  1 x%d" % (pos, c[0], c[1]))
    parity_bad = sum(1 for f in frames if f["ninth"] != bin(f["raw"]).count("1") % 2)
    out.append("frames whose ninth cell differs from even parity of the data: %d of %d" % (parity_bad, len(frames)))
    stops = [f for f in frames if f["stop"] != 1]
    out.append("stop cell (cell 10) low: %d frames" % len(stops))
    return "\n".join(out), msgs, stops


def report_uart(sim, frames):
    same = len(sim) == len(frames) and all(abs(a[0] - b["t"]) < 50 and a[1] == b["raw"] for a, b in zip(sim, frames))
    out = ["fixed 500 baud 8E1 receiver: %d bytes, %d parity errors, %d framing errors; identical bytes to the "
           "per-sender decode: %s" % (len(sim), sum(1 for x in sim if x[2]), sum(1 for x in sim if x[3]), same)]
    return "\n".join(out), same


def describe_messages(msgs, clocks):
    lines = []
    prev_end = None
    t_first = msgs[0][0]["t"]
    for m in msgs:
        sender = m[0]["sender"]
        clock = clocks[sender]
        text = " ".join("%02X" % f["bus"] for f in m)
        spans = [(b["t"] - a["t"]) / clock for a, b in zip(m, m[1:])]
        shape = " ".join("%.0f" % s for s in spans)
        last_end = m[-1]["t"] + CELLS * clock
        gap = "" if prev_end is None else "  gap %.1f ms" % ((m[0]["t"] - prev_end) / 1e3)
        lines.append("%9.4f s  %-6s %-22s byte spans (cells): %s%s" % ((m[0]["t"]) / 1e6, sender, text, shape, gap))
        prev_end = last_end
    return "\n".join(lines)


def check(frames, clocks, msgs, stops):
    problems = []

    def expect(name, ok, got):
        if not ok:
            problems.append("%s: got %s" % (name, got))

    expect("remote bit time 1980-1982 us", 1980 <= clocks["remote"] <= 1982, clocks["remote"])
    expect("unit bit time 2052-2060 us", 2052 <= clocks["unit"] <= 2060, clocks["unit"])
    expect("22 stop-cell violations, all in setting messages",
           len(stops) == 22, len(stops))
    ninth = collections.defaultdict(set)
    for m in msgs:
        v = [f["bus"] for f in m]
        if len(v) == 7 and v[0] == 0x7F and v[1] in (0x22, 0x12):
            for pos, f in enumerate(m):
                ninth[pos].add(f["ninth"])
    expect("ninth cell is a function of position in setting messages",
           all(len(s) == 1 for s in ninth.values()) and "".join(str(next(iter(ninth[p]))) for p in range(7)) == "0011010",
           {p: sorted(s) for p, s in ninth.items()})
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--channel", type=int, default=1, help="bit index of the RX channel (PulseView D1 = 1)")
    ap.add_argument("--messages", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    starts, levels, total = load_samples(args.capture, args.channel)
    frames = annotate(decode_frames(starts, levels, total))
    clocks = sender_clocks(frames)
    text, msgs, stops = report(frames, clocks, total)
    print(text)
    sim = simulate_nominal_uart(starts, levels, total)
    utext, same = report_uart(sim, frames)
    print(utext)
    if args.messages:
        print()
        print(describe_messages(msgs, clocks))
    if args.check:
        problems = check(frames, clocks, msgs, stops)
        if not same:
            problems.append("fixed-500-baud receiver bytes differ from the per-sender decode")
        if problems:
            print("\nCHECK FAILED:\n  " + "\n  ".join(problems))
            sys.exit(1)
        print("\nCHECK PASSED")


if __name__ == "__main__":
    main()
