"""Run the explicit BSC2 lab commands over one serial connection.

write erases and restores the preserved 32MiB GBA backup through the normal
PSRAM/CHIS worker. Firmware requires a successful full verify before a write.
No automatic retries, protocol fallback or cartridge writes after failure.
"""
import argparse
import json
from pathlib import Path
import sys
import time
import zlib

import serial


class Lab:
    def __init__(self, port, log):
        self.port = serial.Serial()
        self.port.port, self.port.baudrate = port, 115200
        self.port.timeout, self.port.write_timeout = 0.2, 10
        self.port.dtr = self.port.rts = False
        self.port.open()
        self.pending = bytearray()
        self.log = Path(log).open("a", encoding="utf-8")

    def wait(self, event, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pending.extend(self.port.read(4096))
            while b"\n" in self.pending:
                raw, _, self.pending = self.pending.partition(b"\n")
                line = raw.decode("utf-8", "replace").rstrip("\r")
                self.log.write(time.strftime("%Y-%m-%d %H:%M:%S ") + line + "\n")
                self.log.flush()
                if "@mori " not in line:
                    if any(word in line for word in ("summary:", "panic", "abort", "Error")):
                        print(line, flush=True)
                    continue
                data = json.loads(line.split("@mori ", 1)[1])
                if data.get("event") == "error":
                    raise RuntimeError(data)
                if data.get("event") == event:
                    if data.get("ok") is False:
                        raise RuntimeError(data)
                    return data
        raise TimeoutError(f"waiting for {event}; inspect device before further action")

    def command(self, command, event, timeout=20):
        self.port.write((command + "\n").encode())
        return self.wait(event, timeout)

    def close(self):
        self.port.close()
        self.log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("backup", "verify", "write", "upload"))
    parser.add_argument("--port", default="COM26")
    parser.add_argument("--file", help="Local AG32 batch for upload")
    parser.add_argument("--label", required=True)
    parser.add_argument("--results", default="docs/cpld_write_bsc2_20260912.jsonl")
    parser.add_argument("--log", default="docs/cpld_write_bsc2_20260912.serial.log")
    args = parser.parse_args()
    result = {"action": args.action, "label": args.label, "started_at": time.strftime("%Y-%m-%dT%H:%M:%S")}
    lab = Lab(args.port, args.log)
    try:
        status = lab.command("status", "status")
        result["device"] = status
        if status["burn_running"] or status["patch_running"]:
            raise RuntimeError("device busy")
        if args.action == "upload":
            payload = Path(args.file).read_bytes()
            import hashlib
            result["sha256"] = hashlib.sha256(payload).hexdigest()
            lab.command(f"bsc2-upload {len(payload)} {zlib.crc32(payload):08x}", "bsc2_upload_ready")
            for offset in range(0, len(payload), 1024):
                chunk = payload[offset:offset + 1024]
                lab.port.write(chunk)
                ack = lab.wait("bsc2_upload_ack")
                if ack["received"] != offset + len(chunk):
                    raise RuntimeError("upload acknowledgement mismatch")
            result["upload"] = lab.wait("bsc2_upload_done")
            result["ok"] = True
        else:
            result["accepted"] = lab.command("bsc2-cart " + args.action, "bsc2_job_started")
            deadline, last_progress = time.monotonic() + 600, 0
            while time.monotonic() < deadline:
                status = lab.command("bsc2-job-status", "bsc2_job_status")
                if time.monotonic() - last_progress >= 10:
                    print(json.dumps(status, ensure_ascii=False), flush=True)
                    last_progress = time.monotonic()
                if not status["running"]:
                    result["job"] = status
                    result["ok"] = (status["state"] == "done" and
                        status["processed"] == status["total"] == 33554432 and
                        status["message"] == {"backup":"dump finished", "verify":"verify finished", "write":"burn finished"}[args.action])
                    break
                time.sleep(1)
            else:
                raise TimeoutError("job still running; inspect before any other action")
            if args.action == "write":
                result["profile"] = lab.command("cpld-write-profile", "cpld_write_profile")
    except Exception as error:
        result.update(ok=False, error=str(error))
    finally:
        lab.close()
    with Path(args.results).open("a", encoding="utf-8") as output:
        output.write(json.dumps(result, ensure_ascii=False) + "\n")
    print(json.dumps(result, ensure_ascii=False), flush=True)
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
