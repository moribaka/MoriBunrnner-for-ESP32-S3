"""Read-only board regression for GBC's patch-free native write menu.

Requires an idle board, a GBC flash cartridge and an existing TF ROM backup.
Analyzes the cartridge and navigates the write submenu without starting a burn.
"""
import argparse
import json
import sys
import time

from test_file_worker import Board


def run(board, rom):
    status = board.cmd("status")
    assert not status["burn_running"] and not status["patch_running"], status
    state = board.ui()
    if state["screen_dimmed"]:
        state = board.key("b")
    for _ in range(8):
        if state["page"] == 0:
            break
        state = board.key("b")
    assert state["page"] == 0, state
    board.select(0, lambda s: "烧录" in s["selection"] or "Burner" in s["selection"], "right")
    state = board.key("a")
    assert state["page"] == 14, state
    board.select(14, lambda s: s["selected"] == 1, "right")
    state = board.key("a")
    assert state["page"] == 15 and state["selected"] == 0, state
    board.key("a")  # Analyze only.
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        state = board.ui()
        if state["count"] == 10:
            break
        time.sleep(0.2)
    assert state["page"] == 15 and state["count"] == 10, state
    board.select(15, lambda s: s["selected"] == 1)
    state = board.key("a")
    assert state["page"] == 6, state
    for part in rom.removeprefix("/sdcard/").split("/"):
        board.select(6, lambda s: s["selection"] == part)
        state = board.key("a")
    assert state["page"] == 15 and state["count"] == 2, state
    board.select(15, lambda s: s["selected"] == 0)
    state = board.key("a")  # MBC5 mapper selection, not write.
    assert state["page"] == 15 and state["count"] == 10, state
    board.select(15, lambda s: s["selected"] == 3)
    state = board.key("a")  # Open write options, never activate its row.
    snapshots = [state]
    for key in ("down", "up", "right", "left", "down", "up"):
        assert state["page"] == 15 and state["count"] == 1 and state["selected"] == 0, state
        assert not any(word in state["selection"] for word in ("WAITCNT", "SRAM", "补丁", "Patch")), state
        state = board.key(key)
        snapshots.append(state)
    assert state["page"] == 15 and state["count"] == 1 and state["selected"] == 0, state
    state = board.key("b")
    assert state["page"] == 15 and state["count"] == 10, state
    for _ in range(8):
        if state["page"] == 0:
            break
        state = board.key("b")
    assert state["page"] == 0 and board.starts == 0, state
    print(json.dumps({"ok": True, "case": "gbc_patch_free_write_menu", "snapshots": snapshots,
                      "final_ui": state}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--log", required=True)
    args = parser.parse_args()
    board = Board(args.port, args.log)
    try:
        run(board, args.rom)
    finally:
        board.serial.close()
        board.log.close()
