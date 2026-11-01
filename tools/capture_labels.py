#!/usr/bin/env python3
"""Log ESPHome output and mark what you did on the remote, in one timestamped file.

Usage (run from the repo root, in a terminal you can type in):

    esphome logs aot30.yaml 2>&1 | python3 tools/capture_labels.py captures/matt-captures/my-test.log

Press the remote button, then type a short label and Enter, e.g. "heat 24" or "fan high".
Each label is written into the log as a line "[HH:MM:SS.mmm][LABEL] heat 24" using the same
clock as the ESPHome log lines (the computer's), so tools/decode_timeline.py can group the
bus messages after each label. Press the button first, then label it, and wait a few seconds
before the next press so the messages do not run together. Ctrl-C to stop.
"""

import sys
import threading
from datetime import datetime

lock = threading.Lock()


def stamp():
    now = datetime.now()
    return now.strftime("%H:%M:%S.") + "%03d" % (now.microsecond // 1000)


def write_line(out, line, echo=True):
    with lock:
        out.write(line + "\n")
        out.flush()
        if echo:
            print(line, flush=True)


def read_labels(out, echo=True):
    try:
        tty = open("/dev/tty")
    except OSError:
        return  # no terminal: logging still works, just without labels
    for text in tty:
        text = text.strip()
        if text:
            write_line(out, "[%s][LABEL] %s" % (stamp(), text), echo=echo)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "a") as out:
        threading.Thread(target=read_labels, args=(out,), daemon=True).start()
        try:
            for line in sys.stdin:
                write_line(out, line.rstrip("\n"))
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
