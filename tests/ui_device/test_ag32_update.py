"""Real-board AG32 regression. Explicit --program authorizes writing the batch.

Non-program mode checks connections and a missing-file failure only. Uses one
serial owner, verifies the TF payload hash, and never retries a failed update.
"""
import argparse
import hashlib
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from test_file_worker import Board


def http(base, path, method="GET", expected=200):
    try:
        with urllib.request.urlopen(urllib.request.Request(base + path, method=method), timeout=10) as response:
            assert expected == 200, response.status
            return json.load(response)
    except urllib.error.HTTPError as error:
        assert error.code == expected, (error.code, error.read())
        return {"rejected": error.code}


def wait_batch(board, expected):
    start = time.monotonic()
    last_print = 0
    while time.monotonic() - start < 900:
        status = board.cmd("ag32-batch-status", "ag32_batch_status")
        if time.monotonic() - last_print > 20:
            print(json.dumps(status), flush=True)
            last_print = time.monotonic()
        if status["state"] != "running":
            assert status["state"] == expected, status
            status["wall_seconds"] = round(time.monotonic() - start, 3)
            if expected == "success":
                assert status["dp_idcode"] == 0x2BA01477 and status["device_id"] == 0x40200001, status
                assert status["records"] == 3 and status["programmed"] == status["verified"] == status["total"] == 108924, status
                assert not status["recovery_required"] and status["mcu_resumed"], status
                assert status["error"] == status["cleanup_error"] == "ESP_OK", status
                assert status["stack_free_min"] >= 2048, status
            return status
        time.sleep(0.4)
    raise TimeoutError("Update still active; do not reset the board")


def native_start(board, filename):
    state = board.ui()
    if state["screen_dimmed"]:
        state = board.key("b")
    for _ in range(8):
        if state["page"] == 0:
            break
        state = board.key("b")
    assert state["page"] == 0, state
    board.select(0, lambda s: s["selection"] in ("系统", "System"), "right")
    state = board.key("a"); assert state["page"] == 1, state
    board.select(1, lambda s: s["selection"] in ("设置", "Settings"), "right")
    state = board.key("a"); assert state["page"] == 17, state
    board.select(17, lambda s: "AG32" in s["selection"])
    state = board.key("a"); assert state["page"] == 20, state
    state = board.key("a"); assert state["page"] == 6, state
    board.select(6, lambda s: s["selection"] == filename)
    state = board.key("a"); assert state["page"] == 20, state
    deadline = time.monotonic() + 15
    while state["selected"] != 4 and time.monotonic() < deadline:
        time.sleep(0.1); state = board.ui()
    assert state["selected"] == 4 and ("开始" in state["selection"] or "Start" in state["selection"]), state
    # Both cancel routes, then the explicit native confirmation.
    state = board.key("a"); assert state["selected"] == 0, state
    state = board.key("b"); assert state["selected"] == 4, state
    board.key("a")
    state = board.key("a"); assert state["selected"] == 4, state
    board.key("a")
    state = board.key("down")
    assert state["selected"] == 1 and "AG32" in state["selection"], state
    board.key("a")


def main(args, board):
    results = {"initial": board.cmd("status"), "probes": [], "updates": []}
    assert not results["initial"]["burn_running"] and not results["initial"]["patch_running"]
    assert board.cmd("ag32-batch-status", "ag32_batch_status")["state"] != "running"
    for _ in range(20):
        result = board.cmd("ag32-probe", "ag32_probe")
        assert result["ok"], result
        results["probes"].append(result)
    board.cmd("ag32-batch /sdcard/missing-ag32-regression-file.bin", "ag32_batch_started")
    result = wait_batch(board, "failed")
    assert result["phase"] == "load" and result["error"] == "ESP_ERR_NOT_FOUND", result
    assert not result["destructive_started"] and not result["recovery_required"], result
    results["missing_file"] = result
    assert board.cmd("ag32-probe", "ag32_probe")["ok"]
    if args.program:
        query = urllib.parse.urlencode({"path": args.batch})
        with urllib.request.urlopen(args.url + "/api/tf/download?" + query, timeout=10) as response:
            image = response.read()
        assert hashlib.sha256(image).hexdigest() == args.sha256.lower()
        check = board.cmd("ag32-batch-check /sdcard/" + args.batch, "ag32_batch_check")
        assert check["ok"] and check["records"] == 3 and check["payload_bytes"] == 108924, check
        for index in range(args.cycles):
            if args.paused_music and index == args.cycles - 1:
                board.cmd("play /sdcard/music/Lazer Boomerang - Signals.mp3", "play")
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    music = http(args.url, "/api/music/status")
                    if music["state"] == "playing": break
                    time.sleep(0.2)
                assert music["state"] == "playing", music
                http(args.url, "/api/music/pause", "POST")
                assert http(args.url, "/api/music/status")["state"] == "paused"
                results["paused_music_memory"] = board.cmd("status")
                print(json.dumps({"paused_music_memory": results["paused_music_memory"]}), flush=True)
            if index == 0 and not args.native_only:
                board.cmd("ag32-batch /sdcard/" + args.batch, "ag32_batch_started")
            else:
                native_start(board, args.batch)
            for key in ("b", "menu", "right"):
                time.sleep(0.3)
                state = board.key(key)
                assert state["page"] == 20, state
            for path, method, code in (
                ("/api/mcu/batch?" + urllib.parse.urlencode({"path": "/sdcard/" + args.batch}), "POST", 409),
                ("/api/tf/list", "GET", 503),
                ("/api/mcu/probe", "GET", 409),
            ):
                http(args.url, path, method, code)
            result = wait_batch(board, "success")
            result["entry"] = "serial" if index == 0 and not args.native_only else "native"
            results["updates"].append(result)
            Path(args.result).write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
            print(json.dumps({"update": index + 1, **result}), flush=True)
            assert board.cmd("cpld-info", "cpld_info")["ok"]
            assert board.cmd("ag32-probe", "ag32_probe")["ok"]
        if args.paused_music:
            http(args.url, "/api/music/stop", "POST")
    results["final"] = board.cmd("status")
    Path(args.result).write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"passed": True, "connections": len(results["probes"]),
        "updates": len(results["updates"])}), flush=True)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--result", required=True)
    parser.add_argument("--program", action="store_true")
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--paused-music", action="store_true")
    parser.add_argument("--native-only", action="store_true")
    parser.add_argument("--batch", default="ag32_cpld_posedge_20260910.bin")
    parser.add_argument("--sha256", default="94efe7052df14813d0829e175ba520f8ee8b5c2b2c6d5a68e17c2604a43b3907")
    args = parser.parse_args()
    board = Board(args.port, args.log)
    try:
        main(args, board)
    finally:
        board.serial.close(); board.log.close()
