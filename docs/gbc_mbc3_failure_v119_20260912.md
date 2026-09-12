# v2.32.119: MBC3 cartridge programmed as MBC5

## Reproduced cause

Board: ESP32-S3 COM26, MAC a4:cb:8f:f2:c4:c0. Single slot contains the user's
MBC3 cartridge with MX29GL256E/F NOR, ID C2 7E 22 01. Selected voltage 5V;
configured/actual SPI 40 MHz. NOR capacity is 32 MiB, but the current standard
MBC3 mapping exposes only 2 MiB. NOR identification does not identify the mapper.

The failed source `自由合卡GBC.gb` is 33,488,896 bytes, SHA256
`2cad76d163d9c610dabe0ff7bc71d2d3e5c5e6407efa9545af44a866e0c3cc03`.
v118 failed reproducibly at the 16 KiB chunk starting at 0x00100000, with both
CPLD streaming and original Bacon. CPLD reported hardware error 8, completed=0.
The previous independent readback first differed at 0x4000 (file FF, cartridge 00).

The native Analyze path only queried NOR CFI/ID, then unconditionally labelled
the cartridge MBC5. Choosing a new ROM cleared the override and opened a mapper
menu with MBC5 selected. A manual MBC5 selection bypassed mapper probing, and
capacity preflight accepted the physical NOR size.

MBC5 bank selection writes low bits to 0x2000 and high bits to 0x3000. On MBC3,
both addresses select the same 7-bit register: the second write resets the bank
to zero (mapped to bank 1). Erase/blank reads and writes consequently alias.
The source has an FF bank at 0x4000, then mostly zero banks through 0xFFFFF;
repeated zero programming to the same physical bank can appear successful.
At 0x100000 the source contains nonzero data (first byte C9), requiring erased
bits which were already programmed to zero. Both transports time out.
Reproduction's smart erase saw erased=1, skipped_blank=47 in the first 6 MiB,
consistent with repeated mapping of the same physical window.

Control before changing firmware: v118 auto-detected MBC3 after reboot, burned
the 2 MiB `口袋妖怪 银.gbc`, and independently verified all 2,097,152 bytes.
Thus the reported write failure is not a v118 CPLD wire-format regression.

## Changes

- Native Analyze now probes MBC bank behavior and reports addressable capacity.
  The mapper menu highlights the detected MBC3 instead of hardcoding MBC5.
- A shared probe reads complete fixed/switched 16 KiB banks using only 0x2000.
  It distinguishes bank-zero remapping from a selectable bank zero, including
  identical 64-byte prefixes. Identical complete banks are explicitly unknown;
  content equality does not silently select MBC5. Such cards require explicit
  mapper selection. This is a probe for the existing MBC3/MBC5 support, not a
  universal mapper detector for arbitrary custom cartridges.
- Conflicting requested/detected types are rejected before erase. Capacity
  preflight applies the mapper limit even without a manual override. The task
  prepare path applies the same selection rules.
- GBA/GBC erase/program algorithms, FF skipping, write paths, CPLD/legacy wire
  protocols and AG32 batch are unchanged. Independent verification stays an
  independent operation; the tests request it explicitly.

## Separate audio memory defect

After music had been initialized, v118 retained its 16 KiB decoder task stack
and I2S DMA while paused for cartridge work. Serial status showed largest internal
allocation 14,336 bytes, below the dump writer's 16,384-byte DMA allocation;
dump failed before cartridge reading. A cold boot with audio uninitialized
completed the same 2 MiB dump.

Burn ownership now cooperatively stops playback, joins the suspended audio
worker, frees its explicitly allocated stack and I2S channel/DMA, and unregisters
decoders. The worker must be suspended and no longer running on its pinned core
before stack reuse. Resume reinitializes under the same adapter mutex as play
and burn ownership, restores 1% volume and the saved position, and is manual.
The playback worker's task control block remains static. No forced deletion of
an active decoder or I2S writer is used.

## Board regression on final app

Final ESP app SHA256:
`C9E7980E33640A54C25E92CCF5EF8CCCB0AF65EF7C2A00F9CFBA66CF19EAB87A`.
Size 2,838,192 bytes. Build version v2.32.119. esptool app flash hash verified;
assets had been flashed and hash verified earlier in this iteration.
AG32 was not flashed; packaged batch remains
`94EFE7052DF14813D0829E175BA520F8EE8B5C2B2C6D5A68E17C2604A43B3907`.

Native/API guard regression selected MBC5 deliberately on this MBC3 card:
HTTP 400, selected=MBC5/detected=MBC3 in serial. Selecting MBC3 and the large
source returned HTTP 400 with available=2097152. Subsequent full verification
of the retained 2 MiB Silver ROM passed; neither rejection erased the cart.

| ROM (2 MiB) | Protocol/path | Erase policy | Burn task | Programming | Dump | Full verify + downloaded dump SHA256 |
| --- | --- | --- | ---: | ---: | ---: | --- |
| Silver | CPLD / PSRAM | smart | 16.380 s | 6.261 s | 2.055 s | pass |
| Pikachu | original Bacon / direct | force | 25.354 s | 14.940 s | 4.091 s | pass |
| Silver | CPLD / pipeline | smart | 16.676 s | 6.264 s | 2.652 s | pass |

These are regression conditions, not controlled cross-protocol speed comparisons:
ROM data, FF density and erase policy differ. Silver SHA256
`04a16b9ce276319b3f6ddbadafdc4d12fc3b6756cbcc02120fa69f1c1bc3a8a2`;
Pikachu SHA256 `48fdc059781e921cd3a648d0bcb52a72b73e5b5b72b85c44d198b655d3089cb2`.
Every downloaded dump matched its source hash.

Silent MP3 -> 2 MiB dump -> blocked play/pause/seek -> completed paused -> manual
resume passed on the final app. Volume remained 1%. Internal free increased
from 16,707 during playback to 45,663 during dump, despite dump owning its DMA.
An earlier test run misconsumed a delayed expected serial rejection and cancelled
its dump; the test now waits for that command's terminal event rather than draining
whatever USB bytes happen to be available after a fixed delay. This was a test
synchronization failure, recorded separately from the successful final run.

Host regressions passed: MBC3/MBC5 probe including blank ambiguity/conflicts/read
errors; audio nested ownership/manual resume; GBC FF spans/bank boundaries/blank
checks; production dump/verify slot lifetime, exact mismatch and cleanup; GBA-only
patch controls; shared status types; AG32 programmer 18 failure/success/snapshot
cases. No GBA or MBC5 cartridge was available for this run; their physical write
regression is not claimed. No 32 MiB MBC3 burn is claimed.

Evidence: `gbc_failure_v118_repro.*`, `gbc_failure_v118_legacy.*`,
`gbc_failure_v118_mbc3_correct.*`, `gbc_mapper_guard_v119.serial.log`,
`gbc_v119_regression/`, and `music_priority_v119_final_pass/`.
Device ends with Silver in the cartridge, auto protocol, music stopped, 1% volume.
