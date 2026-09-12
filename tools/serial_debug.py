"""Mori USB serial diagnostics. Requires pyserial; never toggles reset lines.

Examples:
  python tools/serial_debug.py --port COM26 status
  python tools/serial_debug.py --port COM26 "ls /sdcard"
  python tools/serial_debug.py --port COM26 --timeout 300 "patch sbw /sdcard/game.gba"
"""

import argparse
import json
from pathlib import Path
import sys
import time

import serial


def run(args):
    port = serial.Serial()
    port.port = args.port
    port.baudrate = 115200
    port.timeout = 0.2
    port.write_timeout = 2
    port.dtr = False
    port.rts = False
    log = None
    if args.log:
        log_path = Path(args.log)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log = log_path.open("a", encoding="utf-8")
    try:
        port.open()
        port.reset_input_buffer()
        command = args.command.strip()
        if command != "watch":
            port.write((command + "\n").encode(args.encoding))
        deadline = time.monotonic() + args.timeout
        next_status = time.monotonic() + 2
        pending = bytearray()
        terminal = {
            "status": "status", "ls": "ls_done", "patch": "patch_done", "patch-save": "patch_done",
            "cancel": "cancel", "help": "help", "reboot": "reboot",
            "ui": "ui", "key": "key",
            "epub": "epub_done", "play": "play",
            "tf-bench": "tf_bench_done",
            "ag32-link": "ag32_link", "ag32-ping": "ag32_ping", "ag32-test": "ag32_test",
            "ag32-probe": "ag32_probe", "ag32-batch-check": "ag32_batch_check",
            "ag32-regs": "ag32_regs",
            "ag32-stream-test": "ag32_stream_test",
            "bacon-check": "bacon_check",
            "cpld-info": "cpld_info", "cpld-check": "cpld_check",
            "cpld-read-bench-gba": "cpld_read_bench",
            "cpld-write-experiment": "cpld_write_experiment",
            "cpld-write-profile": "cpld_write_profile", "wifi-connect-saved": "wifi_connect",
            "ag32-batch": "ag32_batch_started", "ag32-batch-status": "ag32_batch_status",
        }.get(command.split(" ", 1)[0])
        while time.monotonic() < deadline:
            pending.extend(port.read(4096))
            while b"\n" in pending:
                raw, _, pending = pending.partition(b"\n")
                line = raw.decode(args.encoding, errors="replace").rstrip("\r")
                print(line, flush=True)
                if log:
                    log.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {line}".rstrip() + "\n")
                    log.flush()
                marker = line.find("@mori ")
                if marker < 0:
                    continue
                try:
                    event = json.loads(line[marker + 6:])
                except json.JSONDecodeError:
                    continue
                if event.get("event") == "error" and command != "watch":
                    return 1
                if event.get("event") == terminal:
                    if event.get("ok") is False:
                        return 1
                    # status.result describes the previous patch, not failure
                    # of the status query itself.
                    return 1 if terminal in ("patch_done", "epub_done") and event.get("result", 0) != 0 else 0
            if terminal == "patch_done" and time.monotonic() >= next_status:
                port.write(b"status\n")
                next_status = time.monotonic() + 2
        if command == "watch":
            return 0
        print("Timed out waiting for reply; command may still be running. Use status to inspect it.", file=sys.stderr)
        return 2
    finally:
        port.close()
        if log:
            log.close()


if __name__ == "__main__":
    # Windows redirected stdout otherwise uses the legacy system code page.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--encoding", default="utf-8", help="FatFS API filename encoding (default: utf-8)")
    parser.add_argument("--log", help="Append timestamped device output to this file")
    parser.add_argument("command")
    try:
        sys.exit(run(parser.parse_args()))
    except (serial.SerialException, UnicodeError) as error:
        print(f"Serial client error: {error}", file=sys.stderr)
        sys.exit(1)
