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
| `status` | Firmware version, uptime, internal/PSRAM heap, burn and patch running flags, patch phase/detail/offset/total/passes, read bytes and timing, and the current AG32 link preference/active backend/capabilities |
| `help` | One-line command summary |
| `cpld-info` | Probe whether the BSC1 CPLD stream engine answers on the bus (enter mode, read identity); reports `ok` and `block_bytes` |
| `cpld-check` | Read 8 x 64 KiB through BSC1 and compare byte-for-byte against a legacy-Bacon reference read. Halts the AG32 MCU for the streaming pass and resumes it. Reports `passes`, `elapsed_us`, `last_first_ready_us`, `last_status_polls` |
| `bacon-check` | 256 legacy single-word reads against a reference with the MCU halted; reports mean/p50/p95/max microseconds |
| `ag32-link` | Query the ESP32-to-AG32 transport preference and active backend |
| `ag32-link [auto\|legacy\|mcu\|cpld]` | Set and persist the transport preference (written to the burn config); refuses while a burn or AG32 batch job is active |
| `ag32-ping` | Negotiate MCU protocol capability without accessing the cartridge bus; reports protocol version, max payload, AG32 clock and identity |
| `ag32-test` / `ag32-stream-test` | 100 echo round-trips at sizes 1..8192 bytes through the MCU transport; the `-stream` variant exercises the streaming call instead of the single-command call |
| `ag32-probe` | Probe AG32 over the board SWD path and then restore 40 MHz SPI |
| `ag32-regs` | Same SWD probe path, reporting a register snapshot instead of programming |
| `ag32-batch-check PATH` | Validate an AG32 batch on TF without programming it; reports record count and payload bytes |
| `ag32-batch PATH` | Program and verify an AG32 batch through onboard SWD; this is destructive |
| `ag32-batch-status` | Report the current or last AG32 batch job: state, phase, processed/total, DP and device IDs, programmed/verified bytes, recovery_required |
| `tf-bench PATH` | Read up to 1 MiB three ways (buffered, 16 KiB buffered, POSIX `read`) and report elapsed time per mode. Read-only comparison, no cartridge access |
| `ui` | UI processing/render counts, metadata polls, directory cache hits, selected item, preview state and file-worker stack headroom |
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

Replies are JSON lines prefixed with `@mori `; each carries an `event` field naming
the reply type (for example `status`, `cpld_check`, `ls_done`, `patch_done`). The
console task reads a 384-byte line buffer and tolerates backspace.

Commands that touch the cartridge, the SPI link or the AG32 refuse to run while a
burn job or an AG32 batch job is active and answer `cartridge or firmware job is
running`. `cpld-check` and `ag32-test` temporarily switch the link preference and
restore the previous value before returning, so they do not disturb the saved
configuration. `cpld-info`, `cpld-check`, `ag32-test` and `ag32-stream-test`
require the AG32 batch job to be idle and take the SPI lock for the duration.

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
