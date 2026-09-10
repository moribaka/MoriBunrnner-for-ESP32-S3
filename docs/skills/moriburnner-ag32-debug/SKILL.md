---
name: moriburnner-ag32-debug
description: Flash and debug the MoriBurnner ESP32-S3 controller and its AG32 companion through onboard SWD, then validate the 40 MHz Bacon/MCU SPI transport. Use for real-board firmware flashing, AG32 batch updates, serial diagnostics, or protocol bring-up.
metadata:
  short-description: ESP32 to AG32 hardware flash/debug
---

# MoriBurnner ESP32 → AG32 hardware debug

Use this skill for the physical `F:\dev\esp32\moriburnner` board workflow. It is an engineering/debug workflow, not a release checklist.

## Hardware identification

- ESP32-S3 native USB Serial/JTAG is normally `COM26`; verify with `esptool chip_id` before flashing.
- Last verified board MAC: `a4:cb:8f:f2:c4:c0`, VID303A/PID1001. COM19 (VIDCAFE) is another device. Discover HTTP IP each session; the observed `192.168.1.134` is DHCP, not a permanent address.
- The companion AG32 is updated by ESP32 over onboard SWD. Never connect an external SWD tool while the ESP32 is in SWD mode.
- Bacon SPI must remain at 40 MHz. Do not lower the clock to hide timing errors.
- Primary cartridge I/O is pure CPLD Bacon. Both legacy and auto select it without MCU negotiation or request allocation. The MCU transport is an explicit experimental option; do not restore MCU as the default after diagnostics.
- AG32 MCU firmware disables only `JTDI`, `JTDO`, and `NJTRST`; `JTCK`/`JTMS` remain available for SWD.
- The AG32 batch is `example/moriburnner_ag32_batch.bin`; validate it before programming.

## ESP32 flash

For a complete ESP32 refresh, run `tools/flash_dual_system.ps1 -Port COM26 -FlashOnly` after the build outputs and `build/flash_args` are current. This writes the bootloader, partition table, app, assets, and fixed Retro-Go partitions.

For an app-only iteration, use the configured ESP-IDF Python and esptool:

```powershell
python -m esptool --chip esp32s3 -p COM26 -b 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x20000 build/moriburnner.bin
```

Always check the esptool `Hash of data verified` line and wait for the native USB Serial/JTAG port to return.

## Serial diagnostics

Use `tools/serial_debug.py` with UTF-8. The client now recognizes AG32 terminal events:

Set DTR/RTS false **before** opening the serial port. Constructing `serial.Serial('COM26', ...)` and then clearing them can reset native USB. Keep one serial owner at a time.

```powershell
python tools/serial_debug.py --port COM26 status
python tools/serial_debug.py --port COM26 ag32-probe
python tools/serial_debug.py --port COM26 ag32-link
python tools/serial_debug.py --port COM26 "ag32-link mcu"
python tools/serial_debug.py --port COM26 ag32-ping
```

`ag32-ping` only negotiates MCU capabilities; it does not access the cartridge bus. A successful probe should report DP IDCODE `0x2ba01477`, and SPI status should report configured/actual 40 MHz.

SPI is initialized lazily. Diagnostics must call `burner_spi_init()` under the SPI lock before transactions. `ESP_ERR_INVALID_STATE` without a wire status is not evidence of failed mode entry. A successful HTTP SWD probe used to hide this bug by initializing SPI on exit.

`ag32-test` checks 100 framed echo transactions from 1 to 8192 bytes. `ag32-stream-test` checks raw streaming echo, including block boundaries and odd final lengths. Neither touches cartridge contents. `ag32-regs` halts the MCU, snapshots transport registers and the first 12 words of each buffer, resumes, and restores SPI. Do not run either SWD probe/snapshot during a flash/burn job. Treat JSON `ok:false` as failure even if the command returned a terminal event. `ag32-link` sets a preference; its active field describes the selected backend, not the instantaneous pin mode.

## AG32 batch update

Upload the batch to the TF root and validate it before starting a destructive update:

```powershell
python -c "import requests; requests.post('http://DEVICE/api/tf/upload?dir=&name=ag32_update.bin', data=open('example/moriburnner_ag32_batch.bin','rb'), timeout=60).raise_for_status()"
Invoke-RestMethod 'http://DEVICE/api/mcu/batch/check?path=/sdcard/ag32_update.bin'
Invoke-RestMethod 'http://DEVICE/api/mcu/batch?path=/sdcard/ag32_update.bin' -Method Post
```

Poll `/api/mcu/batch/status` until a terminal state. A valid success must contain:

- `state=success`
- `dp_idcode=0x2ba01477`
- `device_id=0x40200001`
- `records=3`
- `programmed == verified == total`
- `recovery_required=false`

Older ESP firmware copied the report only at job completion: while `state=running`, zero IDs/counts and `destructive_started=false` were stale defaults, even during erase/program/verify. Never infer that flashing has not started from those old runtime fields. Current job callbacks copy the report with progress; fields are still snapshots, not an atomic observation of the flash controller. Trust terminal success only when all checks above agree. Repeated program/verify phases are normal for separate batch records. Do not interrupt an active update or silently restore SPI after a destructive failure.

## Protocol bring-up order

1. Confirm ESP32 boot, TF mount, selected backend and 40 MHz SPI.
2. For the primary GBA path, run `bacon-check` with legacy or auto selected.
   It compares 256 single-word reads while the MCU is halted against samples
   taken before halting, measures latency, and attempts to resume the MCU.
   Require ok=true and mcu_resumed=true. It is read-only for cartridge contents.
3. Validate/program an AG32 batch only when firmware changes require it.
4. When specifically testing the experimental MCU path, select mcu, then
   require PING/capability, framed echo and stream echo before cartridge writes.
5. Restore the chosen primary CPLD setting after auxiliary MCU diagnostics.

Do not write through the MCU transport while its PING fails. MCU readiness is not a prerequisite for pure-CPLD Bacon operation. Do not add retries, clock reduction, or mid-operation protocol switching to conceal a framing/timing bug.

The CPLD continuous extension is described in shared/BACON_CPLD_STREAM_DESIGN.md
and is still a design, not an implemented bitstream. Do not claim the prior
MCU Stream implements that hardware path.

## Build evidence

For CPLD/interface changes, use the `chisflash-ag32-batch` flow: `pio prelogic`, AG32 release build, `quartus_sh -t af_quartus.tcl`, Supra `af_run.tcl` with `MODE QUARTUS`, `FLOW ALL`, seed 42, `FITTING timing_more`, `EFFORT highest`, then `pio buildbatch`. Record actual logic/tile/LUT/register/BRAM/PLL/pin counts and setup/hold margins. MCU-only builds may reuse the routed CPLD when matching source/settings and bitstream hashes are recorded before and after compilation/packaging.

## Cartridge measurements

Use `tools/ag32_benchmark.py --url URL --action write --link mcu --rom /sdcard/game.gba --verify-rom /sdcard/game.patched.gba --write-path psram --results docs/run.jsonl --port COM26 --label LABEL` for an authorized write comparison. It forces erase, captures serial summaries, and performs full verification. Select legacy/direct/pipeline explicitly; it never retries a failed write through another protocol. The serial port must have no other owner.

For runtime patch tests, first export the matching expected image with `patch-save FLAGS PATH`. The burn uses the original file and matching patch flags; verification uses the exported image. SRAM defaults on in the write API, while `--no-sram` sends `sram=0`, useful for restoring a retained baseline without adding patches. HTTP TF paths are relative to the TF root; the benchmark client normalizes an explicit /sdcard/ prefix.

Use device `task_time_ms` for the burn task and the separate verify task time. Planning/probing before task start is included only in client wall time. NOR program duration comes from the serial summary, captured in `program_reports.program_ms`. Existing HTTP `write_time_ms` measures TF/phase time, not NOR program time. For pipeline erase, use HTTP `erase_time_ms`; the serial summary's erase field covers only the initial synchronous erase. Overlapped component times need not sum to wall time.

## Evidence and regression scope

Read the current project evidence before stating board qualification. SWD batch success, simulated SPI correctness, PING, echo stress, cartridge reads and verified burns are separate milestones.

Model CS pins as separately driven GPIOs. A clockless intermediate CS0-only/CS1-only selection must not create an empty request or restart a response. Status polls must not consume data FIFO entries. Include arbitrary last bits, 8192-byte FIFO wraps, AHB address/data phases and waits, and partial transfers. Native SCK processes require an explicit reset pulse in simulation; a testbench declaration initialized to zero is insufficient for some event-driven reset cases.

Keep the project copy at `docs/skills/moriburnner-ag32-debug/SKILL.md` synchronized. Save source, artifact hashes, actual route timing/utilization, command outcomes and board evidence in local Git. Do not present auto/legacy selection as a separate wire protocol or simulation throughput as measured burn speed.
