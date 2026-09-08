# USB serial diagnostics

The firmware accepts UTF-8 lines on the ESP32-S3 native USB Serial/JTAG port
(115200 baud). Replies are JSON lines prefixed with `@mori `; normal firmware
logs can appear between replies. No Wi-Fi connection is required.

Start with `python tools/serial_debug.py --port COM26 help` (substitute the
device's current COM port). ESP-IDF's Python environment already has pyserial.
The client uses `--encoding utf-8` by default and saves UTF-8 logs. This matches
`CONFIG_FATFS_API_ENCODING_UTF_8`; the separate CP936 OEM codepage setting does
not change the application's path encoding.

| Command | Behavior |
| --- | --- |
| `status` | Firmware version, uptime, heap, active patch phase, file offset, total bytes read, I/O time and result |
| `ui` | UI processing/render counts, metadata polls, directory cache hits and selected item |
| `key up` / `key a` / `key b` | Send a normal UI key press; also supports down/left/right/menu/panel/vol+/vol- |
| `epub /sdcard/book.epub` | Read up to 32 chapters forward/backward and report text hash and ZIP index builds |
| `play /sdcard/music.wav` | Queue an audio file through the regular player |
| `ls /sdcard/path` | List up to 256 directory entries; paths may contain spaces and Chinese characters |
| `patch s /sdcard/game.gba` | Build an SRAM patch plan using the production implementation |
| `patch sb /sdcard/game.gba` | Build SRAM and batteryless plans |
| `patch sbw /sdcard/game.gba` | Include WAITCNT analysis |
| `patch w /sdcard/game.gba` | Analyze WAITCNT only |
| `patch-save sbw /sdcard/game.gba` | Apply the selected patches and save a new sibling `game.patched.gba`; add a number if it already exists |
| `cancel` | Request cooperative cancellation of the current patch plan, including one started by the UI/web |
| `reboot` | Restart the device |

Patch commands run in a separate task and **only build an in-memory plan**.
They do not write the source ROM or erase/program the cartridge. `patch-save`
does write a new ROM file and reports `save_progress` and the final output path.
It removes incomplete output on failure/cancellation. The same operation is
available at the bottom of the GBA burn/patch menu as “打补丁并保存ROM”. Only one
production patch plan/export can run at a time. Cancellation is checked at file-read
boundaries; it cannot interrupt a blocked SD driver operation or undo a burn.
`offset` is the latest read position, not an overall percentage; each new
pattern scan starts at zero and `read_bytes` accumulates across passes.

The host client polls status every two seconds while waiting for a patch:

```powershell
python tools/serial_debug.py --port COM26 --timeout 300 --log .tmp-patch.log "patch sbw /sdcard/game.gba"
```

A host timeout does not cancel the device task. Use `status` or `cancel` after
the client exits. Close other serial monitors before opening this port.
Commands are local physical-access diagnostics, with no network endpoint.
USB MSC mode takes over the shared USB PHY, so disable TF USB pass-through
before using this native USB Serial/JTAG endpoint.
