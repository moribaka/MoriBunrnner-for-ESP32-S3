"""Cancel an in-flight cartridge dump; check workers release TF and buffers."""
import argparse
import json
import sys
import time
import urllib.request
from test_file_worker import Board


def request(url, path, method="GET"):
    with urllib.request.urlopen(urllib.request.Request(url + path, method=method), timeout=15) as response:
        return json.load(response)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", required=True)
    p.add_argument("--port", required=True)
    p.add_argument("--log", required=True)
    args = p.parse_args()
    b = Board(args.port, args.log)
    try:
        before = b.cmd("status")
        assert not before["burn_running"] and not before["patch_running"], before
        accepted = request(args.url, "/api/read?mode=mbc5&size=32MB&name=cancel_check_v112.gbc", "POST")
        assert accepted["ok"], accepted
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            b.ui()
            state = request(args.url, "/api/status")
            assert state["state"] not in ("done", "error", "cancelled"), state
            if state["processed"] >= 65536:
                break
            time.sleep(0.05)
        assert state["processed"] >= 65536, state
        cancel = request(args.url, "/api/cancel", "POST")
        assert cancel["ok"], cancel
        while time.monotonic() < deadline:
            b.ui()
            state = request(args.url, "/api/status")
            if state["state"] == "cancelled":
                break
            time.sleep(0.05)
        assert state["state"] == "cancelled", state
        after = b.cmd("status")
        assert not after["burn_running"], after
        print(json.dumps({"ok": True, "accepted": accepted, "cancelled": state,
                          "before": before, "after": after}), flush=True)
    finally:
        b.serial.close()
        b.log.close()
