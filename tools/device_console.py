#!/usr/bin/env python3
"""Run in the foreground in tmux using ESP-IDF's Python environment.

Append 'live start', 'live stop', 'live mute', 'live unmute', or 'live status'
to --commands from another shell. No credential provisioning occurs over USB.
"""

import argparse
import os
import time
from pathlib import Path

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--commands", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=600)
    args = parser.parse_args()
    os.umask(0o077)
    args.commands.touch(mode=0o600)
    allowed = {
        "live start",
        "live stop",
        "live mute",
        "live unmute",
        "live status",
        "live timing",
        "live audio-test",
        "live wifi-scan",
        "live wifi-reconnect",
        "live wifi-next",
        "live screen",
        "live screenshot",
        "live panel-reset",
    }
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    pending = ""
    deadline = time.monotonic() + args.seconds
    try:
        with args.log.open("ab", buffering=0) as log, args.commands.open() as commands:
            commands.seek(0, os.SEEK_END)
            print("USB console ready; commands:", args.commands, flush=True)
            while time.monotonic() < deadline:
                data = port.read(max(1, port.in_waiting))
                if data:
                    log.write(data)
                    print(data.decode(errors="replace"), end="", flush=True)
                pending += commands.read()
                while "\n" in pending:
                    line, pending = pending.split("\n", 1)
                    if line.strip() in allowed:
                        port.write((line.strip() + "\n").encode())
                        print("\nUSB command:", line.strip(), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        port.close()


if __name__ == "__main__":
    main()
