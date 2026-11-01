#!/usr/bin/env python3
"""Capture device logs straight from the ESPHome native API, without the esphome CLI.

Setup (once):  pip install aioesphomeapi

Usage, from the repo root, in a terminal you can type in:

    python3 tools/api_log_capture.py heatpump.local captures/matt-captures/my-test.log [--key KEY] [--password PW] [--quiet]

--key is the API encryption key from the device's `api: encryption: key:` setting, if it has
one (HA's ESPHome add-on shows it in the device YAML). Only one client may use the log
stream at a time on some setups; close the HA log viewer for this device if the connection
is refused.

Output lines match the esphome CLI format ("[HH:MM:SS.mmm][I][tag]: text") using this
computer's clock. Type a label and Enter after each remote press, as with
tools/capture_labels.py; labels use the same clock. Use --quiet to keep the live log from
scrolling in the terminal while still saving it to the file. Ctrl-C to stop. Reconnects
if the device drops off the network.

Not yet tested against a real device.
"""

import argparse
import asyncio
import re
import sys
import threading

import aioesphomeapi

from capture_labels import read_labels, stamp, write_line

ANSI = re.compile(r"\x1b\[[0-9;]*m")


async def run(args, out):
    client = aioesphomeapi.APIClient(args.host, args.port, args.password, noise_psk=args.key)
    while True:
        disconnected = asyncio.Event()

        async def on_stop(expected_disconnect):
            disconnected.set()

        def on_log(message):
            text = ANSI.sub("", message.message.decode("utf-8", errors="replace"))
            for line in text.splitlines():
                write_line(out, "[%s]%s" % (stamp(), line), echo=not args.quiet)

        try:
            await client.connect(on_stop=on_stop, login=True)
            client.subscribe_logs(on_log, log_level=aioesphomeapi.LogLevel.LOG_LEVEL_DEBUG)
            write_line(out, "[%s][CAPTURE] connected to %s" % (stamp(), args.host))
            await disconnected.wait()
            write_line(out, "[%s][CAPTURE] disconnected, retrying" % stamp())
        except aioesphomeapi.APIConnectionError as err:
            write_line(out, "[%s][CAPTURE] connection failed: %s" % (stamp(), err))
        await asyncio.sleep(3)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host")
    parser.add_argument("logfile")
    parser.add_argument("--port", type=int, default=6053)
    parser.add_argument("--key", default=None, help="API encryption key (base64)")
    parser.add_argument("--password", default=None, help="legacy API password")
    parser.add_argument("--quiet", action="store_true", help="save device logs to file without echoing them")
    args = parser.parse_args()

    with open(args.logfile, "a") as out:
        if args.quiet:
            print("Quiet capture: logs are saved to %s. Type a label and Enter after each remote action." % args.logfile,
                  flush=True)
        threading.Thread(target=read_labels, args=(out,), daemon=True).start()
        try:
            asyncio.run(run(args, out))
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    sys.exit(main())
