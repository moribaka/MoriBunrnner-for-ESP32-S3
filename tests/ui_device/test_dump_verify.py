"""Native 4 MiB dump and full reference verification; never writes a cartridge."""
import argparse
import json
import sys
import time
import urllib.request
from test_file_worker import Board


def wait_dump(b, url):
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        ui = b.ui()
        with urllib.request.urlopen(url + "/api/status", timeout=10) as response:
            status = json.load(response)
        if status["state"] in ("done", "error", "cancelled"):
            assert status["state"] == "done" and status["message"] == "dump finished", status
            assert status["processed"] == status["total"], status
            return {"status": status, "ui": ui}
        time.sleep(0.2)
    raise TimeoutError("Native dump did not finish")


def run(b, args):
    status = b.cmd("status")
    assert not status["burn_running"] and not status["patch_running"], status
    state = b.ui()
    if state["screen_dimmed"]:
        state = b.key("b")
    for _ in range(8):
        if state["page"] == 0:
            break
        state = b.key("b")
    b.select(0, lambda s: "烧录" in s["selection"] or "Burner" in s["selection"], "right")
    assert b.key("a")["page"] == 14
    b.select(14, lambda s: s["selected"] == (args.mode == "mbc5"), "right")
    assert b.key("a")["page"] == 15
    b.select(15, lambda s: s["selected"] == 0)
    b.key("a")
    count = 12 if args.mode == "mbc5" else 9
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        state = b.ui()
        if state["page"] == 15 and state["count"] == count:
            break
        time.sleep(0.2)
    assert state["count"] == count, state
    b.select(15, lambda s: s["selection"] in ("ROM: 读取", "ROM: Dump"))
    state = b.key("a")
    b.select(15, lambda s: s["selection"] == "4 MiB")
    b.key("a")
    dumped = wait_dump(b, args.url)
    assert dumped["status"]["total"] == 4 * 1024 * 1024, dumped
    assert b.key("b")["page"] == 15
    b.select(15, lambda s: s["selected"] == (2 if args.mode == "mbc5" else 1))
    assert b.key("a")["page"] == 6
    for part in args.rom.removeprefix("/sdcard/").split("/"):
        b.select(6, lambda s: s["selection"] == part)
        state = b.key("a")
    if args.mode == "mbc5":
        assert state["page"] == 15 and state["count"] == 2, state
        b.select(15, lambda s: s["selected"] == 0)
        state = b.key("a")
    assert state["page"] == 15 and state["count"] == count, state
    b.select(15, lambda s: s["selection"] in ("ROM: 校验", "ROM: Verify"))
    b.key("a")
    verified = b.wait_job(args.url, "verify finished")
    state = b.key("b")
    for _ in range(8):
        if state["page"] == 0:
            break
        state = b.key("b")
    assert state["page"] == 0, state
    print(json.dumps({"ok": True, "mode": args.mode, "dump": dumped,
                      "verify": verified, "memory": b.cmd("status")}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True)
    p.add_argument("--url", required=True)
    p.add_argument("--mode", choices=("gba", "mbc5"), required=True)
    p.add_argument("--rom", required=True)
    p.add_argument("--log", required=True)
    args = p.parse_args()
    b = Board(args.port, args.log)
    try:
        run(b, args)
    finally:
        b.serial.close()
        b.log.close()
