"""Board regression for the native ROM preview/start worker.

Requires an idle MoriBurnner, pyserial, and the named ROMs in the TF root.
Immediate/replace/sleep use ROM verification only; startup is tested separately
from cartridge content. The explicit burn case writes --write-rom (32 MiB);
the caller must first preserve and verify the target cartridge's backup.
Export writes a new patched TF file without modifying the cartridge.
"""
import argparse
import json
import sys
import time
from pathlib import Path
import urllib.parse
import urllib.request

import serial


class Board:
    def __init__(self, port, log):
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = 115200
        self.serial.timeout = 0.1
        self.serial.dtr = self.serial.rts = False
        self.serial.open()
        self.log = Path(log).open("a", encoding="utf-8")
        self.starts = 0

    def line(self):
        line = self.serial.readline().decode("utf-8", errors="replace").strip()
        if line:
            self.log.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {line}\n")
            self.log.flush()
        if "LCD start file action:" in line:
            self.starts += 1
            self.started_at = time.monotonic()
            print(line, flush=True)
        if "ROM preview generation=" in line:
            print(line, flush=True)
        assert "create start task failed" not in line, line
        assert "Stack canary" not in line and "Guru Meditation" not in line, line
        return line

    def cmd(self, command, event=None):
        self.serial.write((command + "\n").encode())
        event = event or command.split()[0]
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            line = self.line()
            if "@mori " in line:
                data = json.loads(line.split("@mori ", 1)[1])
                assert data.get("event") != "error", data
                if data.get("event") == event:
                    return data
        raise TimeoutError(command)

    def ui(self):
        return self.cmd("ui")

    def key(self, key):
        self.cmd("key " + key)
        time.sleep(0.06)
        return self.ui()

    def select(self, page, predicate, direction="down"):
        state = self.ui()
        assert state["page"] == page, state
        for _ in range(state["count"] + 1):
            if predicate(state):
                return state
            state = self.key(direction)
            assert state["page"] == page, state
        raise AssertionError(state)

    def enter_burner(self):
        status = self.cmd("status")
        assert not status["burn_running"] and not status["patch_running"], status
        if self.ui()["screen_dimmed"]:
            self.key("b")
        state = self.ui()
        if state["page"] == 15 and state["count"] == 9:
            return state
        for _ in range(8):
            if state["page"] == 0:
                break
            state = self.key("b")
        assert state["page"] == 0, state
        self.select(0, lambda s: "烧录" in s["selection"] or "Burner" in s["selection"], "right")
        state = self.key("a")
        assert state["page"] == 14, state
        if state["selected"] != 0:
            state = self.key("left")
        assert state["selected"] == 0, state
        state = self.key("a")
        assert state["page"] == 15 and state["selected"] == 0, state
        self.key("a")  # Analyze only: never erase/unlock/program.
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            state = self.ui()
            if state["count"] == 9 and state["selection"].startswith("ROM:"):
                return state
            time.sleep(0.2)
        raise AssertionError(state)

    def choose(self, filename):
        self.select(15, lambda s: s["selected"] == 1)
        state = self.key("a")
        assert state["page"] == 6, state
        for part in filename.removeprefix("/sdcard/").split("/"):
            self.select(6, lambda s: s["selection"] == part)
            state = self.key("a")
        assert state["page"] == 15 and state["selected"] == 2, state
        return state

    def verify_start(self):
        state = self.key("down")
        assert state["page"] == 15 and state["selected"] == 3, state
        assert "校验" in state["selection"] or "Verify" in state["selection"], state
        before = self.starts
        requested_at = time.monotonic()
        self.key("a")
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            state = self.ui()
            status = self.cmd("status")
            if self.starts > before and not state["file_start_active"] and not status["burn_running"]:
                assert state["file_worker_stack_free"] >= 2048, state
                result = {"start_ms": round((self.started_at - requested_at) * 1000),
                          "ui": state, "memory": status}
                print(json.dumps(result, ensure_ascii=False), flush=True)
                state = self.key("b")
                assert state["page"] == 15, state
                return result
            time.sleep(0.2)
        # Stop a long read-only verification before failing the test.
        self.cmd("cancel")
        raise AssertionError(state)

    def wait_job(self, url, expected, timeout=600):
        deadline = time.monotonic() + timeout
        next_progress = 0
        while time.monotonic() < deadline:
            state = self.ui()  # Drain serial and retain stack/worker evidence.
            with urllib.request.urlopen(url + "/api/status", timeout=15) as response:
                status = json.load(response)
            if time.monotonic() >= next_progress:
                print(json.dumps({k: status[k] for k in ("state", "progress", "processed", "message")}), flush=True)
                next_progress = time.monotonic() + 20
            if not state["file_start_active"] and status["state"] in ("done", "error", "cancelled"):
                assert status["state"] == "done" and status["message"] == expected, status
                assert status["processed"] == status["total"], status
                assert state["file_worker_stack_free"] >= 2048, state
                return {"status": status, "ui": state}
            time.sleep(0.2)
        raise TimeoutError("Job still active; inspect status before another operation")


def run(args, board):
    board.enter_burner()
    results = []
    if args.case == "burn":
        assert args.write_rom, "burn requires an explicit --write-rom and a verified backup"
        board.choose(args.write_rom)
        state = board.key("a")
        assert state["page"] == 15 and state["count"] == 5 and state["selected"] == 0, state
        before = board.starts
        board.key("a")
        result = board.wait_job(args.url, "burn finished")
        assert board.starts > before, "native worker did not start"
        assert result["status"]["total"] == 32 * 1024 * 1024, result
        results.append(result)
    elif args.case == "export":
        board.choose(args.short_rom)
        state = board.key("a")
        assert state["page"] == 15 and state["count"] == 5, state
        board.select(15, lambda s: s["selected"] == 1)
        state = board.key("a")
        assert state["count"] == 3, state
        board.select(15, lambda s: s["selection"] == "SRAM")
        state = board.key("a")
        assert state["count"] == 5 and state["selected"] == 1, state
        board.key("down")
        state = board.key("a")
        assert "yes" in state["status"].lower() or "开启" in state["status"] or "是" in state["status"], state
        board.key("down")
        state = board.key("a")
        assert "yes" in state["status"].lower() or "开启" in state["status"] or "是" in state["status"], state
        state = board.key("down")
        assert state["selected"] == 4 and state["count"] == 5, state
        board.key("a")
        results.append(board.wait_job(args.url, "补丁ROM已保存"))
    elif args.case == "replace":
        for _ in range(3):
            state = board.choose(args.long_rom)
            assert state["preview_active"] and not state["preview_done"], state
        generation = state["preview_generation"]
        state = board.choose(args.short_rom)
        assert state["preview_generation"] > generation, state
        deadline = time.monotonic() + 30
        while state["preview_active"] and time.monotonic() < deadline:
            time.sleep(0.2)
            state = board.ui()
        assert state["preview_done"] and state["preview_available"], state
        print(json.dumps({"replacement": state, "memory": board.cmd("status")}, ensure_ascii=False), flush=True)
    elif args.case == "sleep":
        deadline = time.monotonic() + 185
        state = board.ui()
        while not state["screen_dimmed"] and time.monotonic() < deadline:
            time.sleep(1)
            state = board.ui()
        assert state["screen_dimmed"] and state["page"] == 15, state
        print("Screen dimmed in burner; waking and selecting ROM", flush=True)
        state = board.key("b")
        assert state["page"] == 15 and not state["screen_dimmed"], state
        board.choose(args.long_rom)
        results.append(board.verify_start())
    else:
        for _ in range(3):
            state = board.choose(args.long_rom)
            assert state["preview_active"], state
            results.append(board.verify_start())
    print(json.dumps({"case": args.case, "passed": True, "results": results}, ensure_ascii=False), flush=True)
    if args.result:
        Path(args.result).write_text(json.dumps({"case": args.case, "passed": True,
            "results": results}, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--case", choices=("immediate", "replace", "sleep", "burn", "export"), required=True)
    parser.add_argument("--url", help="Current device HTTP URL, required for burn/export")
    parser.add_argument("--write-rom")
    parser.add_argument("--result")
    parser.add_argument("--long-rom", default="LK_MULTIMENU_L556.gba")
    parser.add_argument("--short-rom", default="黄金太阳1 开启的封印.gba")
    args = parser.parse_args()
    if args.case in ("burn", "export") and not args.url:
        parser.error("burn/export requires --url from the current device session")
    board = Board(args.port, args.log)
    try:
        run(args, board)
    finally:
        board.serial.close()
        board.log.close()
