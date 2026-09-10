"""Native same-data burn finishes at 100% without automatic ROM verification."""
import argparse
import json
import sys
import time
from test_file_worker import Board

if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url",required=True)
    p.add_argument("--port",required=True)
    p.add_argument("--rom",required=True)
    p.add_argument("--mode",choices=("gba","mbc5"),default="mbc5")
    p.add_argument("--log",required=True)
    args=p.parse_args()
    b=Board(args.port,args.log)
    try:
        assert not b.cmd("status")["burn_running"]
        if args.mode=="gba":
            b.enter_burner()
            b.choose(args.rom)
            assert b.key("a")["count"]==5
        else:
            s=b.ui()
            if s["screen_dimmed"]: s=b.key("b")
            for _ in range(8):
                if s["page"]==0: break
                s=b.key("b")
            b.select(0,lambda s:"烧录" in s["selection"] or "Burner" in s["selection"],"right")
            assert b.key("a")["page"]==14
            b.select(14,lambda s:s["selected"]==1,"right")
            assert b.key("a")["page"]==15
            b.select(15,lambda s:s["selected"]==0)
            b.key("a")
            deadline=time.monotonic()+30
            while time.monotonic()<deadline:
                s=b.ui()
                if s["count"]==12: break
                time.sleep(0.2)
            assert s["count"]==12,s
            b.select(15,lambda s:s["selected"]==2)
            assert b.key("a")["page"]==6
            for part in args.rom.removeprefix("/sdcard/").split("/"):
                b.select(6,lambda s:s["selection"]==part)
                s=b.key("a")
            assert s["page"]==15 and s["count"]==2,s
            b.select(15,lambda s:s["selected"]==0)
            b.key("a")
            b.select(15,lambda s:s["selection"] in ("ROM: 写入","ROM: Write"))
            assert b.key("a")["count"]==1
        b.key("a")
        result=b.wait_job(args.url,"burn finished")
        status=result["status"]
        assert status["progress"]==100 and not status["verify_sample_valid"],status
        assert not any(k in status for k in ("write_verification_planned","write_verified_bytes","verify_time_ms")),status
        assert status["write_matched_bytes"]==status["total"] and status["erase_sector_count"]==0,status
        s=b.key("b")
        for _ in range(8):
            if s["page"]==0: break
            s=b.key("b")
        assert s["page"]==0,s
        print(json.dumps({"ok":True,"result":result},ensure_ascii=False),flush=True)
    finally:
        b.serial.close();b.log.close()
