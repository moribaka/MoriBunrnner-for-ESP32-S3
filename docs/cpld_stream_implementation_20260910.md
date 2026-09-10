# CPLD stream implementation / active bring-up

Source baseline:1b4f7f9. The user's separate annotation in
AG32_实机测试对比_20260910.md is preserved and is not part of this change.

## Implemented, not yet board-qualified

- Pure CPLD descriptor parser, address cursor, GBA/GB read and raw write,
  standard AMD buffered-program microsequence and local ready polling.
- Two1024-byte RAM buffers. Receive/CRC of the next page overlaps programming
  the current page. Read buffers reserve4bytes for CRC, yielding1020-byte
  read blocks. SPI packets contain no repeated address or size.
- Twenty-byte BSC1 descriptor, byte addresses/count, page mask (page bytes-1),
  CRC32; raw payload+padding+CRC32; four dummy bytes before read payload.
- Status16bytes: dummy4, BA CE 01 flags, completed bytes LE32, error LE32.
- Safe hardware mode exit waits for RD/WR pulses to finish. No MCU/AHB data
  path in this firmware. Experimental MCU transport source remains behind
  ENABLE_MCU_TRANSPORT, disabled in the pure-CPLD build due physical capacity.
- ESP32 transport, CPLD preference, read/program hooks and cpld-info/check.
  cpld-check compares repeated64KiB reads while the MCU is halted. Explicit
  CPLD jobs probe support before erasing; unsupported write recipes fail early.

Icarus at40MHz / sys150MHz passes GBA/GB reads, odd tails, raw writes, AMD
unaligned first-page boundaries,1024-byte pages, CRC errors, overlong/short
packets, read truncation, NOR timeout, double-buffer overlap and safe abort.
Mode guard and legacy power compatibility pass, including the optional MCU
profile. Host descriptor byte order and CRC checks pass. Simulations do not
substitute for actual board or routed timing validation.

## Resource/timing iterations

1. All three engines: Quartus required215 LAB regions, available132; rejected.
2. Remove experimental MCU data plane and redundant address/count storage:
   Quartus fits. Supra2090/2112 logic,131/132tiles; worst system setup-15.951ns
   from dynamic page/length calculations to receive control. Not flashed.
3. Pipeline RX page/length calculations and completion updates: full132tiles,
   congested routing still unresolved after approximately9minutes; stopped
   this superseded candidate, not packaged.
4. Reuse read RAM for CRC, use a single iterative CRC register; sequential
   FSM encoding was worse (133/132tiles), rejected. Restored normal encoding.
5. Replace page-size subtraction with page-mask arithmetic, reuse header
   registers, remove redundant bit count. Latest routed result is pending.

Commands, in example then example/logic, sequentially:
pio run -e release -t prelogic; pio run -e release;
quartus_sh -t af_quartus.tcl (includes af_ip.tcl);
af_run.bat -X 'set MODE QUARTUS' -X 'set FLOW ALL' -X 'set SEED42' with
FITTING timing_more / FITTER hybrid / EFFORT highest / HOLDX default / SKEW basic.
Detailed current logs:example/logic/quartus_cpld.log and supra_cpld.log.
Do not package an earlier generated example_board.bin while newer routing
is incomplete or failed. Never use example_board_batch.bin (dummy MCU).

## Current physical cartridge and preserved baseline

The board currently reports ID89 00 7E 22 28 22 01 22, CFI AMD128MiB,
128KiB sectors,1024-byte programming buffer. This differs from the earlier
512-byte-buffer comparison; use same-card fresh measurements for comparison.
Original data matched all32MiB of the Pokemon source named in the JSON log.

Backup:/sdcard/ROM_OUTPUT/cpld_before_20260910.gba. Legacy dump113639ms,
bus read26700ms. Forced legacy PSRAM write of that backup passed, with full
readback verification; serial summary erase37328ms, program43633ms, total81229ms
(task adds setup), verify33827ms. Data remains identical to the original card.
Evidence:docs/cpld_benchmark_0910.jsonl and its serial.log sibling.

New CPLD bitstream has not been flashed, and no new-protocol hardware speed
claim is made at this checkpoint. Keep SPI40MHz and sys/MCU150MHz.
