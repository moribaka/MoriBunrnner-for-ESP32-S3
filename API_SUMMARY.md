# MORI Burner - API Summary (Code Synced)

Last synchronized with: `main/ws_server.c`

## 1. Runtime Features

- Device: ESP32-S3 + TF card + Web panel + SPI2 cartridge backend.
- Dual web entry:
  - Base settings page: `GET /`, `GET /sys`
  - Business entry: `GET /tf`, `/cart`, `/burner`, `/settings` -> `/sdcard/.web/main.html`
- Static resource mapping: non-API GET -> `/sdcard/.web/<path>` via `GET /*`.
- ROM write pipeline (current):
  - Data backend is split by mode:
    - `mode=mbc5` -> MBC5/GBC ROM pipeline
    - `mode=gba` -> GBA ROM pipeline
  - Supports two write paths:
    - `write_path=direct`: `TF -> RAM chunk -> cart`
      - MBC5 write loop uses 4KB chunks
      - GBA write loop uses 64KB chunks
    - `write_path=psram`: `TF -> PSRAM(N MB pipeline) -> cart`
      - `N = psram_mb` (1..8, default 4)
      - Per-window loop: erase current window + prefetch TF in parallel, then program from PSRAM
  - Upload source files to TF with `/api/upload`
- GBA power sequence (single-CS extension):
  - Before GBA operations, firmware issues: **5V off -> 3V3 on**
- MBC5 power sequence (single-CS extension):
  - Before MBC5 operations, firmware issues: **all rails off -> 5V on**
- ROM read pipeline (current):
  - Trigger dump task with `/api/read`
  - `mode=mbc5`: 16KB outer chunk
  - `mode=gba`: 64KB outer chunk (size clamp to flash end like host `mission_dumpRom`)
- ROM verify pipeline:
  - Trigger verify task with `/api/verify`
  - `mode=mbc5`: direct range verify
  - `mode=gba`: 64KB chunk compare + odd-byte file pad behavior + size clamp like host `mission_verifyRom`
- RAM pipeline (MBC5):
  - Write: `/api/ram/write`
  - Dump: `/api/ram/read`
  - Verify: `/api/ram/verify`
- USB pass-through guard:
  - While TF is occupied by USB host, TF-related APIs return `503 Service Unavailable`.

## 2. Page Routes

- `GET /` -> built-in base settings page
- `GET /sys` -> built-in base settings page
- `GET /tf` -> `/sdcard/.web/main.html`
- `GET /cart` -> `/sdcard/.web/main.html`
- `GET /burner` -> `/sdcard/.web/main.html`
- `GET /settings` -> `/sdcard/.web/main.html`
- `GET /*` -> static file from `/sdcard/.web`

## 3. HTTP API Index

## 3.1 Burn/Dump APIs

- `GET /api/status`
  - Response JSON fields: `state`, `progress`, `processed`, `total`, `speed_current_bps`, `speed_avg_bps`, `speed_min_bps`, `speed_max_bps`, `speed_warmup_ms`, `tf_to_psram_speed_current_bps`, `tf_to_psram_speed_avg_bps`, `tf_to_psram_speed_min_bps`, `tf_to_psram_speed_max_bps`, `dump_read_speed_current_bps`, `dump_read_speed_avg_bps`, `dump_read_speed_min_bps`, `dump_read_speed_max_bps`, `dump_write_speed_current_bps`, `dump_write_speed_avg_bps`, `dump_write_speed_min_bps`, `dump_write_speed_max_bps`, `erase_time_ms`, `write_time_ms`, `dump_read_time_ms`, `dump_write_time_ms`, `spi_configured_hz`, `spi_actual_hz`, `cart_auto_sleep_ms`, `cart_sleeping`, `cancel_requested`, `rom`, `message`
  - Speed stats (`avg/min/max`) ignore the first `speed_warmup_ms` (currently `1000`) after write phase starts
  - `tf_to_psram_speed_*` is the dedicated `TF -> PSRAM` transfer speed (valid when `write_path=psram`)
  - `dump_read_speed_*` / `dump_write_speed_*` show direct ROM dump phase speeds for `cart -> RAM` and `RAM -> TF`
  - `dump_read_time_ms` / `dump_write_time_ms` show cumulative elapsed time for those two direct dump phases
  - `state`: `idle|receiving|burning|done|error|cancelled`

- `POST /api/cancel`
  - Sends a cancel request for the current receive / burn / dump / verify / erase / TF upload / Web upload / system deploy / OTA flow
  - Firmware stops at the next safe checkpoint and reports `state=cancelled`

- `POST /api/upload?name=<file_name>[&mode=mbc5|gba]`
  - Uploads a browser-selected ROM file to `/sdcard/roms`
  - `mode=gba` auto-pads odd-sized ROM by 1 byte to keep even alignment
  - Status transitions to `receiving` during upload

- `POST /api/write?name=<file_name>[&mode=mbc5|gba][&slot=...][&write_path=direct|psram][&psram_mb=1..8]`
  - Starts ROM write task from an existing TF file
  - `mode=mbc5`:
    - `slot=0..17`, follows host MBC5 multi-cart range table (`0` = whole cart)
  - `mode=gba`:
    - `slot=0`: whole card base
    - `slot=1`: multi-cart menu base
    - `slot>=2`: base = `(8 + 4*(slot-2)) MB` (host `mission_gba.cs`)
  - `write_path=direct`: direct `TF -> cart`
  - `write_path=psram`: `TF -> PSRAM(N MB pipeline) -> cart`, looping erase+prefetch+program per window
  - `psram_mb=1..8` (optional, default `4`): PSRAM staging window size in MB, used when `write_path=psram`
  - Success: `{"ok":true,"mode":"gba","write_path":"psram","psram_mb":4,"message":"burn started"}`

- `POST /api/read?name=<file_name>&size=<bytes|KB|MB>[&mode=mbc5|gba][&slot=...]`
  - Starts ROM dump task
  - Dump path is fixed: cartridge -> RAM chunk -> final TF file directly
  - `read_path` is accepted only for compatibility; ROM dump always uses direct TF output
  - `mode=mbc5`: `slot=0..17`
  - `mode=mbc5`: ROM export uses a Bacon-style streaming ROM read path
  - `mode=gba`: slot rule matches host GBA multi-cart selector
  - `mode=gba`: ROM export uses Bacon hoststyle read timing
  - `size` examples: `33554432`, `32K`, `32M`, `32MB`
  - Output path: `/sdcard/ROM_OUTPUT/<file_name>`
  - If the target file already exists, firmware auto-appends a timestamp and numeric suffix to avoid overwrite
  - No temporary fragment merge is used for ROM dump

- `POST /api/verify?name=<file_name>[&mode=mbc5|gba][&slot=...]`
  - Starts ROM verify task with an existing file from `/sdcard/roms` or `/sdcard/dumps`
  - `mode=mbc5`: ROM verify uses the same Bacon-style streaming ROM read path as dump

- `POST /api/ram/write?name=<file_name>[&slot=0..17][&ram_type=sram|fram][&ram_latency=0..255]`
  - Starts RAM write task with an existing file from `/sdcard/roms` or `/sdcard/dumps`

- `POST /api/ram/read?name=<file_name>&size=<bytes|KB|MB>[&slot=0..17][&ram_type=sram|fram][&ram_latency=0..255]`
  - Starts RAM dump task: cartridge RAM -> TF file

- `POST /api/ram/verify?name=<file_name>[&slot=0..17][&ram_type=sram|fram][&ram_latency=0..255]`
  - Starts RAM verify task with an existing file from `/sdcard/roms` or `/sdcard/dumps`
  - Defaults: `ram_type=sram`; FRAM latency default is `10`

- `POST /api/cart/erase[?mode=mbc5|gba]`
  - `mode=mbc5`: MBC5 chip erase
  - `mode=gba`: GBA chip erase (includes S70GL02 dual-bank host behavior)

- `GET /api/cart/id[?mode=gba|mbc5]`
  - Reads cartridge flash ID
  - `mode=gba`: applies 5V off -> 3V3 on
  - `mode=mbc5`: applies all off -> 5V on
  - Returns: `id`, `chip`, `cfi_ok`, `device_size`, `sector_size`, `buffer_write`

- `GET /api/cart/id_debug[?mode=gba|mbc5][&sample_addr=<uint32>][&sample_len=1..64]`
  - Debug-only read-ID variant; keeps `/api/cart/id` unchanged
  - Reads cartridge flash ID and then reads a normal ROM sample in the same request
  - Default sample: `sample_addr=0`, `sample_len=32`
  - If sample read fails, ID response still returns with `sample_ok=false` and `sample_error`
  - Returns original ID fields plus: `sample_ok`, `sample_addr`, `sample_len`, `sample_hex`, `sample_error`

## 3.2 TF File APIs

- `GET /api/tf/list?path=<relative_path>`
- `POST /api/tf/upload?dir=<relative_dir>&name=<file_name>` (body: octet-stream)
- `GET /api/tf/download?path=<relative_file_path>`
- `DELETE /api/tf/delete?path=<relative_path>`
- `POST /api/tf/rename?from=<relative_path>&to=<relative_path>`
- `POST /api/tf/mkdir?path=<relative_dir_path>`

## 3.3 Removed APIs

- Removed: `GET /psram`
- Removed: `POST /api/psram/bench?...`
- Removed: `GET /api/ip5306/ini`
- Removed: `POST /api/ip5306/ini`

## 3.4 Device APIs

- `GET /api/device/info` (text)
- `POST /api/device/restart`
- `GET /api/device/brightness`
- `POST /api/device/brightness` (JSON body: `{"brightness":0..255}`)
- `GET /api/spi/config`
- `POST /api/spi/config?mhz=<20..80>` (or `hz=<20000000..80000000>`)
- `GET /api/burn/core_config`
- `POST /api/burn/core_config?erase=auto|cpu0|cpu1&tf=auto|cpu0|cpu1&psram=auto|cpu0|cpu1`

## 3.5 Power APIs

- `GET /api/power/status`
- `POST /api/power/charge_current` (always `403`, firmware-fixed current)

## 3.6 Storage Control APIs

- `GET /api/storage/status`
- `POST /api/storage/usb_msc?enable=1|0`

## 3.7 Language APIs

- `GET /api/lang`
- `GET /api/lang/list`
- `POST /api/lang/apply?ini=<file.ini>` (also supports JSON body `{"ini":"file.ini"}`)

## 3.8 Wi-Fi APIs

- `GET /api/wifi/status`
- `GET /api/wifi/scan`
- `POST /api/wifi/connect` (JSON body: `{"ssid":"...","password":"...","save":true|false}`)
- `POST /api/wifi/ap`
- `POST /api/wifi/disconnect`
- `POST /api/wifi/forget`

## 3.9 Firmware/Web Asset APIs

- `POST /api/web/main_html[?name=main.html]`
- `POST /api/web/upload?name=<file_name>`
- `POST /api/fw/upgrade`
- `GET /api/system/migrate_zip`
  - Exports a migration ZIP that contains `/sdcard/.setting` and `/sdcard/.web`
  - Download file name: `mori_system_migration.zip`
- `POST /api/system/deploy_zip` (body: zip binary)
  - Deploys ZIP content into `/sdcard/.setting` and `/sdcard/.web`
  - Accepted ZIP constraints: standard central directory + `store` method only (no deflate/encryption)

## 3.10 MCU Probe API

- `GET /api/mcu/probe?seq=...&delay=...&norst=...&swap=...&ta=...&ipull=...`

## 4. Path and Safety Rules

- TF paths are normalized under `/sdcard`
- Path traversal (`..`) is rejected
- Invalid FAT characters are rejected
- TF/business APIs are blocked while USB pass-through is active

## 5. Build Note

- Use ESP-IDF initialized terminal if `idf.py` is missing
- If `ninja -C build` fails with toolchain launch errors, fix local `ccache/gcc` path first
