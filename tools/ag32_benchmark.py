"""Run one explicitly selected cartridge job and preserve measured device status.

Write jobs are destructive. The caller selects the ROM, protocol and recipe;
this tool never retries a failed write or changes protocols after an error.
Use an exported patched ROM as --verify-rom when testing runtime patches.
"""
import argparse
import json
import re
from pathlib import Path
import sys
import time
import threading
import urllib.parse
import urllib.request


def capture_serial(port_name, path, stop, reports):
    import serial
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = port_name, 115200, 0.2
    port.dtr = port.rts = False
    try:
        port.open()
        with path.open("a", encoding="utf-8") as output:
            while not stop.is_set():
                line = port.readline().decode("utf-8", errors="replace")
                if line:
                    output.write(time.strftime("%Y-%m-%d %H:%M:%S ") + line)
                    output.flush()
                    if any(marker in line for marker in ("summary:", "program profile:", "panic", "abort")):
                        print(line.rstrip(), flush=True)
                    match = re.search(r"GBA \S+ summary: err=(\S+) total=(\d+)ms bytes=(\d+)/(\d+) erase=(\d+)ms tf=(\d+)ms program=(\d+)ms", line)
                    if match:
                        reports.append(dict(zip(("error", "total_ms", "programmed", "total_bytes",
                            "erase_ms", "tf_ms", "program_ms"),
                            (match[1], *(int(match[i]) for i in range(2, 8))))))
    finally:
        port.close()


def request(base, path, params=None, method="GET"):
    url = base.rstrip("/") + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, method=method)
    with urllib.request.urlopen(req, timeout=180) as response:
        result = json.load(response)
    if result.get("ok") is False:
        raise RuntimeError(result)
    return result


def job(args, action, rom):
    params = {"mode": "gba", "name": rom.removeprefix("/sdcard/"), "recipe_mode": args.recipe}
    if action == "write":
        params.update(write_path=args.write_path, pipeline_erase="force",
                      waitcnt=int(args.waitcnt), batteryless=int(args.batteryless))
    if action == "read":
        params.update(size="32MB", dump_chunk_kb=64)
    start = time.monotonic()
    accepted = request(args.url, "/api/" + action, params, "POST")
    print(json.dumps({"action": action, "accepted": accepted}, ensure_ascii=False), flush=True)
    deadline = start + args.timeout
    next_progress = 0
    expected_message = {"write": "burn finished", "read": "dump finished",
                        "verify": "verify finished"}[action]
    while time.monotonic() < deadline:
        status = request(args.url, "/api/status")
        if time.monotonic() >= next_progress:
            print(json.dumps({"action": action, **{k: status.get(k) for k in
                ("state", "progress", "processed", "task_time_ms", "message")}},
                ensure_ascii=False), flush=True)
            next_progress = time.monotonic() + 20
        if status["state"] in ("error", "cancelled"):
            return {"ok": False, "action": action, "accepted": accepted,
                    "status": status, "wall_seconds": time.monotonic() - start}
        if status["state"] == "done" and status["message"] == expected_message:
            return {"ok": status["processed"] == status["total"], "action": action,
                    "accepted": accepted, "status": status,
                    "wall_seconds": time.monotonic() - start}
        time.sleep(1)
    raise TimeoutError("Job is still active; inspect device status before any further action")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--action", choices=("read", "verify", "write"), required=True)
    parser.add_argument("--link", choices=("legacy", "mcu"), required=True)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--verify-rom")
    parser.add_argument("--write-path", choices=("direct", "psram", "pipeline"), default="psram")
    parser.add_argument("--recipe", choices=("chis", "chislink", "gbx"), default="chis")
    parser.add_argument("--waitcnt", action="store_true")
    parser.add_argument("--batteryless", action="store_true")
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--results", required=True)
    parser.add_argument("--port", help="Optional sole-owner serial capture during this run")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    result = {"configuration": vars(args), "started_at": time.strftime("%Y-%m-%dT%H:%M:%S")}
    stop = threading.Event()
    capture = None
    reports = []
    if args.port:
        log_path = Path(args.results).with_suffix(".serial.log")
        log_path.parent.mkdir(parents=True, exist_ok=True)
        capture = threading.Thread(target=capture_serial, args=(args.port, log_path, stop, reports))
        capture.start()
    try:
        current = request(args.url, "/api/status")
        if current["state"] in ("burning", "receiving"):
            raise RuntimeError("A cartridge job is already active")
        result["ag32_firmware"] = request(args.url, "/api/mcu/batch/status")
        if result["ag32_firmware"]["state"] == "running":
            raise RuntimeError("An AG32 firmware update is already active")
        request(args.url, "/api/burn/core_config", {"ag32_link": args.link}, "POST")
        result["spi"] = request(args.url, "/api/spi/config")
        if result["spi"].get("actual_hz") != 40000000:
            raise RuntimeError("Expected actual SPI clock 40000000 Hz")
        result["job"] = job(args, args.action, args.rom)
        result["ok"] = result["job"]["ok"]
        if args.action == "write" and result["ok"]:
            result["verify"] = job(args, "verify", args.verify_rom or args.rom)
            result["ok"] = result["verify"]["ok"]
    except Exception as error:
        result.update(ok=False, error=str(error))
    finally:
        stop.set()
        if capture:
            capture.join()
        result["program_reports"] = reports
    path = Path(args.results)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as output:
        output.write(json.dumps(result, ensure_ascii=False) + "\n")
    print(json.dumps(result, ensure_ascii=False), flush=True)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
