"""Capture native USB logs across reset without clearing RX or toggling reset pins."""
import argparse
import time
from pathlib import Path
import serial

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--port', required=True)
p.add_argument('--log', required=True)
p.add_argument('--seconds', type=float, default=600)
a = p.parse_args()
Path(a.log).parent.mkdir(parents=True, exist_ok=True)
deadline = time.monotonic() + a.seconds
with open(a.log, 'ab', buffering=0) as log:
    while time.monotonic() < deadline:
        port = serial.Serial()
        port.port = a.port
        port.baudrate = 115200
        port.timeout = 0.2
        port.dtr = False
        port.rts = False
        try:
            port.open()
            print('CAPTURE OPEN ' + a.port, flush=True)
            while time.monotonic() < deadline:
                data = port.read(4096)
                if data:
                    log.write(data)
        except serial.SerialException as error:
            print('CAPTURE RECONNECT ' + str(error), flush=True)
            time.sleep(0.5)
        finally:
            port.close()
