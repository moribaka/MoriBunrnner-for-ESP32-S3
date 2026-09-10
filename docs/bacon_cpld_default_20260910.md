# Primary path restored to pure CPLD Bacon

The requested low-latency/high-throughput primary path is Bacon in the CPLD.
The default in firmware, new TF configuration and UI is legacy. An existing
auto setting also selects CPLD directly. MCU I/O requires explicit selection.
Non-MCU write/read helpers return before allocating or packing MCU requests.
Legacy packet encoding and the AG32 CPLD bitstream were not changed here.

ESP32 app and assets were built/flashed with hash verification. The TF web UI
was confirmed to match the project's pre-MCU UI revision, then updated with
the current backend selector. It no longer labels MCU as the high-speed path.
Board settings were saved as legacy, PSRAM; SPI remains configured/actual40MHz.

## Hardware proof and latency

New serial command: bacon-check. It uses the production single-word GBA read
function, not a synthetic echo. It reads64 distinct word addresses, checks
that the sampled data varies, halts the AG32 MCU over SWD, and then compares
256 timed reads with those reference words. It attempts to resume the MCU
after the check. Read timing excludes SWD entry/exit, power setup and logging.
ESP32 uses its existing160MHz performance lock; no clocks were increased.

| Setting / conditions | Samples | Mean | P50 | P95 | Max | Data | MCU resumed |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| legacy, during boot/Wi-Fi startup | 256 | 48.05us | 36us | 37us | 2775us | match | yes |
| auto, after startup | 256 | 37.00us | 36us | 37us | 131us | match | yes |

The MCU was halted during both timed runs. This verifies the runtime data
path does not require MCU instruction execution. Latency includes ESP32
software/SPI-driver cost and scheduling jitter; it is not just SPI wire time.
These are read-only, single-word checks, not new 32MiB write benchmarks.
No cartridge content was changed during this task.

Build and affected web JavaScript/status-polling checks passed. The separately
edited user annotation in AG32_实机测试对比_20260910.md was left untouched.
Raw board evidence: bacon_cpld_default_0910.log.

## Continuous extension status

shared/BACON_CPLD_STREAM_DESIGN.md defines the intended hardware path:
one address/length descriptor, an incrementing CPLD cursor, raw data blocks,
page-level NOR execution and real receive/program overlap. The extension is
not implemented in the current bitstream. Existing small accesses retain
the one-transaction Bacon encoding and require no MCU handshake.
