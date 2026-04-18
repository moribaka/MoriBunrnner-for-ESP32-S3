# Bacon Host Behavior

## Scope

This note only dissects the Bacon Windows host application under:

- `H:\dev\esp32\Bacon\...\ChisFlashBurner`

It does **not** cover:

- `bacon.v`
- ESP32-side reimplementation details
- electrical guesses outside what the host code explicitly does

The goal is to extract the host-side behavior model so the ESP side can later mirror it faithfully.

Source files used:

- `utility.cs`
- `Form1.cs`
- `CartAdapter_bacon.cs`
- `mission_gba.cs`
- `mission_mbc5.cs`
- `mission_tools.cs`

## 1. Overall layering

The host is organized in three layers:

1. `Form1.cs`
   UI event handlers. Buttons do not directly talk to CH347. They start background worker threads.
2. `mission_*.cs`
   Workflow layer. This is where "read ID", "erase", "program ROM", "dump save", "set RTC", and similar user actions are implemented.
3. `CartAdapter_bacon.cs`
   Transport and packet layer. This is the real Bacon protocol packing for CH347 SPI.

The effective call chain is:

`button click -> ThreadStart(mission_*) -> bacon_* primitive -> CH347SPI_*`

This separation matters because later ESP work should copy the **transport contract** from `CartAdapter_bacon.cs`, not just imitate the high-level mission names.

## 2. UI entry points in Form1

All long-running operations are launched on worker threads.

### GBA tab

- `mission_readRomID()`
- `mission_eraseChip()`
- `mission_programRom()`
- `mission_dumpRom()`
- `mission_verifyRom()`
- `mission_wrtieSram()`
- `mission_dumpRam()`
- `mission_verifyRam()`
- `mission_writeSave_batteryless()`
- `mission_dumpSave_batteryless()`
- `mission_verifySave_batteryless()`
- `mission_rumbleTest_gba()`
- `mission_unlockPPB_gba()`
- `mission_setRTC_gba()`

### MBC5 / GBC tab

- `mission_readRomID_mbc5()`
- `mission_eraseChip_mbc5()`
- `mission_programRom_mbc5()`
- `mission_dumpRom_mbc5()`
- `mission_verifyRom_mbc5()`
- `mission_wrtieRam_mbc5()`
- `mission_dumpRam_mbc5()`
- `mission_verifyRam_mbc5()`
- `mission_unlockPPB_mbc5()`
- `mission_setRTC_mbc3()`

### Important UI routing detail

The shared "read ID" button is routed by selected tab:

- GBA tab -> `mission_readRomID()`
- MBC5 tab -> `mission_readRomID_mbc5()`

So Bacon treats GBA and GBC/MBC5 as two different host protocols from the top level down. They are not the same transport with different addresses.

## 3. CH347 initialization and runtime mode

### 3.1 Operational open path

`utility.cs/openPort()` is the real runtime configuration used for work:

- `ChipMode` must be `1` or `2`
- `iMode = 0` -> SPI mode 0
- `iClock = 0` -> `60 MHz`
- `iByteOrder = 1` -> MSB first
- `iChipSelect = 0`
- `CS1Polarity = 0` -> active low
- `CS2Polarity = 0` -> active low
- `iIsAutoDeativeCS = 0` -> do not auto-release CS

This is a key extracted fact:

- Bacon host does **not** lower runtime SPI frequency for normal operations
- normal work path is explicitly configured at `60 MHz`

### 3.2 Enumeration / probe path

`Form1.cs` contains a separate CH347 SPI init during port enumeration with:

- `iClock = 5` -> `1.875 MHz`

But that path is only for device probing / UI display, not the real cartridge operation path.

So if later behavior comparison is made against Bacon, the correct reference is:

- host runtime work path = `60 MHz`
- not the enumeration helper path

## 4. CH347 CS presets used by Bacon host

`CartAdapter_bacon.cs` does manual `CH347SPI_SetChipSelect()` before every logical transaction, then releases both CS lines after the whole transaction is finished.

The `spi_cs` argument used by Bacon is **not** a direct hardware pin name. It is just a small set of host-side presets:

| `spi_cs` | CH347 output value | Effective state |
| --- | --- | --- |
| `0` | `0x0100` | `CS1=0`, `CS2=1` |
| `1` | `0x0001` | `CS1=1`, `CS2=0` |
| `2` | `0x0000` | `CS1=0`, `CS2=0` |

Observed usage in host code:

- `spi_cs = 0`
  GBA ROM path and GBA RAM path transport calls
- `spi_cs = 1`
  power / control path using `optionByte1`
- `spi_cs = 2`
  GBC optimized transport using `optionByte2`

Important conclusion:

- host code distinguishes between outer CH347 CS preset and inner Bacon bus control bits
- many bus phase changes happen inside the packet stream via `optionByte*`, not by repeatedly toggling CH347 CS between every word

This is one of the biggest differences between "works like Bacon" and "looks vaguely similar".

## 5. Transport rules in CartAdapter_bacon.cs

### 5.1 One logical transaction keeps CH347 CS stable

`bacon_ch347Write()` and `bacon_ch347WriteRead()` both follow the same pattern:

1. assert one outer CH347 CS preset
2. split the byte stream into packets of at most `2048` bytes
3. send each chunk with `CH347SPI_WriteRead()`
4. only after the whole logical transaction, release both CH347 CS lines

Important extracted rule:

- Bacon does **not** deassert outer CH347 CS between the internal chunks of one logical transaction
- chunking is only a CH347 transfer-size workaround, not a protocol boundary

### 5.2 Write and read both use `CH347SPI_WriteRead`

Even pure write paths do not use `CH347SPI_Write()` in the active code. The host uses:

- `CH347SPI_WriteRead()` for write-only logical transfers
- `CH347SPI_WriteRead()` for read transfers as well

This is important when cloning behavior exactly.

### 5.3 Option byte formats

The host packet format is explicitly encoded into helper functions:

#### `optionByte0`

Used for classic Bacon control words:

- bits `[7:6]` -> `batch_size`
- bit `5` -> `dir_a`
- bit `4` -> `dir_ad`
- bit `3` -> `cs2`
- bit `2` -> `cs1`
- bit `1` -> `rd`
- bit `0` -> `wr`

This is the main format for:

- GBA ROM read/write
- GBA RAM read/write
- parts of GBC flash command sequences

#### `optionByte1`

Used for power / control:

- bit `6` -> `power_en`
- bit `5` -> `power_5v`
- bit `4` -> `power_3v`
- bit `2` -> `dir_cs2`
- bits `[1:0]` -> `phi_div`

This is only sent via `spi_cs = 1`.

#### `optionByte2`

Used for the GBC optimized stream:

- bits `[7:6]` -> `batch_size`
- bit `5` -> `dir_a`
- bit `4` -> `dir_ad`
- bit `3` -> `ad_incr`
- bit `2` -> `cs1`
- bit `1` -> `rd`
- bit `0` -> `wr`

Relative to `optionByte0`, this format replaces explicit `cs2` control with `ad_incr`, making it better suited to byte-stream reads/writes with automatic address increment.

### 5.4 Power sequencing

`bacon_power()` behavior is fixed:

1. always send power-off first
2. sleep `100 ms`
3. if enabling:
   - enable `5V`, then sleep `100 ms`, or
   - enable `3.3V`, then sleep `100 ms`

So Bacon power switching is never a direct "flip to target voltage" operation. It is:

- off first
- wait
- new voltage on
- wait

## 6. GBA transport behavior

### 6.1 Addressing model

GBA flash command sequences are word-based:

- command addresses like `0x555` and `0x2AA` are word addresses
- ROM reads in mission code are often byte-addressed
- code converts between the two where needed

This is critical. A transport that gets read data correctly can still fail all command-mode operations if word-address command packing is wrong.

### 6.2 Base ROM primitives

Active GBA low-level primitives:

- `bacon_romWrite(UInt32 wordAddr, ...)`
- `bacon_romRead(UInt32 addr_byte, ref byte[] buf)`
- `bacon_romReadID(ref byte[] id)`
- `bacon_romProgram(UInt32 addr, byte[] buf, UInt16 bufferWriteBytes)`

Behavior pattern for ROM write/read packets:

- set base address first
- hold the logical bus
- use inner `cs1/rd/wr` transitions inside `optionByte0`
- release bus only at the end

### 6.3 GBA ID read sequence

`bacon_romReadID()` performs standard unlock/autoselect:

1. `0x555 <- 0xAA`
2. `0x2AA <- 0x55`
3. `0x555 <- 0x90`
4. read words:
   - `0x000`
   - `0x001`
   - `0x00E`
   - `0x00F`
5. reset with `0xF0`

The mission layer then interprets the resulting 8-byte ID and recognizes:

- `S29GL256`
- `JS28F256`
- `S29GL01`
- `S70GL02`
- `MX29GL256`
- `M29W256G`

### 6.3.1 GBA read-ID exact transaction breakdown

This is the most important detail if ESP must clone Bacon exactly.

`bacon_romReadID()` is not one long combined packet. It is eight independent logical SPI transactions:

1. `bacon_romWrite(0x555, 0x00AA)`
2. `bacon_romWrite(0x2AA, 0x0055)`
3. `bacon_romWrite(0x555, 0x0090)`
4. `bacon_romRead(0x000 << 1)`
5. `bacon_romRead(0x001 << 1)`
6. `bacon_romRead(0x00E << 1)`
7. `bacon_romRead(0x00F << 1)`
8. `bacon_romWrite(0x000, 0x00F0)`

For each of those eight logical transactions:

- outer CH347 CS preset is `spi_cs = 0`
- CS is asserted once before the whole logical transaction
- CS is released once after the whole logical transaction
- there is no host-side explicit delay between transaction N and N+1

So the explicit inter-step delay model in Bacon host is:

- after power-off command: `100 ms` if `bacon_power()` was called
- after power-on command: `100 ms` if `bacon_power()` was called
- between `0xAA -> 0x55 -> 0x90 -> reads -> 0xF0`: `0 ms` explicit delay

Important nuance:

- GBA `mission_readRomID()` itself does not call `bacon_power()`
- therefore, inside the host GBA read-ID workflow, the autoselect sequence itself contains no `Sleep()` at all
- any delay seen in practice comes only from the transfer time of those eight transactions and normal host/API overhead

Packet sizes for the eight steps:

- each single-word `bacon_romWrite()` builds an `11-byte` packet
- each single-word `bacon_romRead()` builds a `10-byte` packet

That means a host GBA read-ID call consists of:

- `4` write transactions x `11` bytes
- `4` read transactions x `10` bytes

Total protocol bytes shifted on SPI:

- `44 + 40 = 84 bytes`

Since Bacon CH347 chunking is `2048` bytes, none of these steps are chunk-split. Each step is exactly one CH347 transfer call for that logical transaction.

#### Single-word GBA write packet shape

For a one-word write like `0x555 <- 0x00AA`, Bacon builds:

1. occupy bus and send 24-bit word address
2. establish base address with `cs1=0`
3. send 16-bit data word
4. pulse `wr` low
5. return `wr` high
6. release bus with `cs1=1`

Concrete packet length:

- `4` bytes address phase
- `1` byte base-establish phase
- `5` bytes data/write-strobe phase
- `1` byte release phase
- total `11 bytes`

#### Single-word GBA read packet shape

For a one-word read like `read word 0x000`, Bacon builds:

1. occupy bus and send 24-bit base word address
2. establish base address with `cs1=0`
3. drive `dir_ad` to input and pull `rd` low
4. clock out one 16-bit word
5. release bus with `cs1=1`

Concrete packet length:

- `4` bytes address phase
- `1` byte base-establish phase
- `4` bytes read phase
- `1` byte release phase
- total `10 bytes`

#### What "exactly matching Bacon" means for ESP

If ESP is supposed to be faithful to Bacon host, the read-ID path should preserve all of the following:

- use GBA word addresses `0x555`, `0x2AA`, `0x555`
- send `0x00AA`, `0x0055`, `0x0090` as 16-bit little-endian words
- perform four separate reads at words `0x000`, `0x001`, `0x00E`, `0x00F`
- send reset `0x00F0` after the reads, even on error
- keep the sequence as eight independent logical transactions
- do not insert extra protocol-level idle commands between these steps
- do not insert extra host-side settle delay between these steps

### 6.4 GBA CFI read

`gba_romGetSize()` does:

1. `bacon_romWrite(0x55, 0x98)`
2. read a CFI window starting at `(0x27 << 1)`
3. reset with `0xF0`

Extracted geometry:

- `deviceSize`
- `bufferWriteBytes`
- `sectorCount`
- `sectorSize`

### 6.5 GBA program path

`mission_programRom()` does the following:

1. load ROM file
2. if length is odd, pad to even length
3. read CFI / flash geometry
4. determine multicart base address
5. if needed, check first `512` bytes at target region
6. if target is not blank, erase affected sectors
7. program in chunks up to `65536` bytes

`bacon_romProgram()` has two active modes:

- single-word programming when `bufferWriteBytes == 0`
- buffered word programming when `bufferWriteBytes > 0`

Important extracted fact:

- the active GBA programming code path is the `optionByte0` path
- the commented experimental alternatives are **not** the live host behavior

### 6.6 GBA dump and verify

`mission_dumpRom()` and `mission_verifyRom()` both use:

- chunk size up to `65536`
- bank switching when working on multicart devices

This leads to an important diagnostic rule:

- if GBA dump works but GBA ID / CFI / program fail, the raw read path may still be correct
- the likely mismatch is then in command-entry or write-sequence packing, not necessarily the plain ROM read path

### 6.7 GBA bank switching and multicart

Host-side helpers:

- `gba_sramSwitchBank()`
- `gba_romSwitchBank()`
- `gba_romSwitchBank_byAddr()`
- `gba_flashSwitchBank()`
- `gba_multiCardBaseAddr()`

Observed rules:

- ROM multicart operations may switch banks around `32 MB` regions
- some menu selections map into `4 MB` windows
- GBA save flash bank switching uses standard flash bank-switch command flow
- SRAM save bank switching uses `0x800000`

### 6.8 GBA save path

Save types are selected by UI:

- `SRAM`
- `FLASH`
- `FRAM`

Workflow behavior:

- write/dump/verify are chunked at `4096` bytes
- bank switch at:
  - `0x00000` -> bank 0
  - `0x10000` -> bank 1

Type-specific behavior:

- `SRAM`
  direct `bacon_ramWrite()` / `bacon_ramRead()`
- `FLASH`
  chip erase first, then `bacon_ramFlashProgram()`
- `FRAM`
  uses `bacon_ramWrite_forFram()` / `bacon_ramRead_forFram()`

Important extracted detail:

- the FRAM helpers are only aliases to the normal RAM helpers in the host
- they do not implement a fundamentally different transport format

### 6.9 Batteryless save path

Batteryless save is not normal RAM access. It is a ROM-area workflow.

`gba_searchBatteryless()` behavior:

1. get ROM geometry
2. read boot vector from ROM
3. derive payload search base from boot vector
4. read two `4096` byte windows
5. search for ASCII marker:
   - `"<3 from Maniac"`
6. parse payload size and save size
7. compute ROM offset where batteryless save lives

Then:

- `mission_writeSave_batteryless()` erases ROM sectors and programs save into ROM space
- `mission_dumpSave_batteryless()` reads that ROM area back
- `mission_verifySave_batteryless()` compares ROM area against file

This means Bacon host has two totally different GBA save models:

- normal save device path
- batteryless save embedded inside ROM path

## 7. GBC / MBC5 transport behavior

### 7.1 GBC uses a different packet style

The stable GBC path is based on:

- `bacon_gbc_write()`
- `bacon_gbc_read()`
- `bacon_gbc_romProgram()`

Active implementation characteristics:

- uses `optionByte2`
- uses address auto-increment
- uses `spi_cs = 2`

There is older/commented `optionByte0` style code nearby, but the active GBC optimized path is the `optionByte2` version.

This is the real reason GBC feels structurally cleaner on the host:

- it is wrapped into a dedicated byte-stream transport
- not because GBC and GBA are secretly the same protocol

### 7.2 MBC5 bank switching

Helpers:

- `mbc5_romSwitchBank(int bank)`
- `mbc5_ramSwitchBank(int bank)`

Bank control writes:

- ROM bank low byte -> `0x2000`
- ROM bank high bit -> `0x3000`
- RAM bank -> `0x4000`

There are explicit ROM/RAM address-range tables at the top of `mission_mbc5.cs` for multicart layout.

### 7.3 MBC5 ID and CFI

`mbc5_romGetID()`:

1. `0xAAA <- 0xAA`
2. `0x555 <- 0x55`
3. `0xAAA <- 0x90`
4. read:
   - `0x00`
   - `0x02`
   - `0x1C`
   - `0x1E`
5. reset `0xF0`

`mbc5_romGetSize()`:

- enters CFI with `bacon_gbc_write(0xAA, 0x98)`
- reads geometry from byte-oriented CFI offsets
- resets with `0xF0`

Recognized devices include:

- `MX29LV640EB`
- `MX29LV640ET`
- `S29GL256N`
- `JS28F256`

### 7.4 MBC5 power behavior

Unlike GBA missions, MBC5 missions explicitly use `bacon_power()`:

- before ROM/RAM operations they usually switch to `5V`
- after finishing they usually fall back to `3.3V`

Since `bacon_power()` itself always does `power off -> wait -> enable target voltage -> wait`, that exact sequence is part of Bacon host behavior.

### 7.5 MBC5 program / dump / verify

Observed chunk sizes:

- ROM program -> `4096`
- ROM dump -> `16384`
- ROM verify -> `16384`
- RAM write/dump/verify -> `4096`

Special case:

- `JS28F256` forces `bufferWriteBytes = 256`

### 7.6 MBC5 RAM path

Key rules:

- RAM enable uses `0x0000 <- 0x0A`
- RAM bank selection uses `0x4000`
- FRAM mode uses `bacon_gbc_*_forFram()`

Important extracted detail:

- `bacon_gbc_write_forFram()` and `bacon_gbc_read_forFram()` are also only aliases to normal GBC transport in the host

So for both GBA and GBC, FRAM is treated as a policy choice at the mission layer, not as a separate underlying Bacon packet format.

## 8. Tools and extension behaviors

### 8.1 PPB unlock

`mission_unlockPPB_gba()` and `mission_unlockPPB_mbc5()` do the same class of work on different buses:

- reset command state
- read CFI / geometry
- read global PPB lock state
- enumerate sector PPB states
- optionally perform `All PPB Erase`

Command form differs between GBA and GBC paths, but the mission logic is equivalent.

### 8.2 GBA RTC

`mission_setRTC_gba()` uses GPIO-like registers mapped in ROM space:

- `0xC4`
- `0xC6`
- `0xC8`

Behavior:

- enable GPIO mode
- bit-bang S3511-style serial traffic
- read status
- optionally reset / set time

This is not a normal save or ROM flow. It is a GPIO-over-ROM-register control path.

### 8.3 MBC3 RTC

`mission_setRTC_mbc3()` uses classic MBC3 RTC register access:

- latch via `0x6000`
- select RTC register via `0x4000`
- access register range `0x08` to `0x0C`

This is a completely different RTC access model than GBA.

### 8.4 GBA rumble

`mission_rumbleTest_gba()` uses the same GBA-side GPIO-style mechanism to pulse rumble-related control lines.

Again, this is a special GPIO behavior, not plain ROM/SRAM traffic.

## 9. Why GBC can look fine while GBA fails

From the host code alone, the strongest structural explanation is:

1. GBC uses a dedicated optimized transport wrapper:
   `optionByte2 + auto-increment + spi_cs=2`
2. GBA command-mode operations use a different wrapper:
   `optionByte0 + word-address command sequences + spi_cs=0`
3. GBA dump mostly validates the read primitive
4. GBA ID / CFI / erase / program validate write-entry and command-mode sequencing

So these observations can all be true at the same time:

- GBA dump works
- CS1 electrical read path is fine
- GBA ID still fails

That combination points more toward:

- wrong GBA command framing
- wrong word/byte address handling
- wrong hold/release timing across the logical transaction

and less toward:

- raw read bus failure
- "CS1 is broken" as a general statement

## 10. Rules to preserve when cloning Bacon behavior onto ESP

If the ESP side is supposed to behave like Bacon host, the following rules appear fundamental:

1. Keep GBA and GBC as two distinct transport contracts.
   Do not merge them into one generic packet path unless the packet builder still reproduces both exact wire behaviors.
2. Preserve Bacon outer-CS behavior.
   One logical transaction asserts one CH347 CS preset, streams possibly multiple `2048` chunks, then releases only once.
3. Preserve the active runtime speed reference.
   Bacon host work path is configured for `60 MHz`.
4. Preserve GBA word-address command sequences exactly.
   Dump success does not prove command-mode correctness.
5. Preserve GBC optimized wrapping exactly.
   Stable GBC behavior comes from dedicated `optionByte2` framing, not from sharing GBA packet structure.
6. Treat FRAM as a mission-layer alias, not a fundamentally different low-level transport.
7. Preserve the power state machine for MBC5/GBC.
   Bacon does `off -> wait -> target voltage -> wait`, not an instantaneous voltage swap.

## 11. Practical reimplementation boundary for ESP

If later this needs to be mirrored on ESP, the cleanest split is:

- `transport_power`
  mirror `optionByte1`
- `transport_gba_rom_read`
- `transport_gba_rom_write`
- `transport_gba_ram_read`
- `transport_gba_ram_write`
- `transport_gbc_read`
- `transport_gbc_write`
- `transport_gba_rom_program`
- `transport_gbc_rom_program`

And then keep the mission layer separate:

- read ID
- read CFI
- erase
- program
- dump
- verify
- save
- RTC
- PPB
- rumble

That boundary matches the Bacon host architecture closely.

## 12. Final extracted summary

The Bacon host is not just "send some SPI bytes to a cartridge". It is a layered protocol implementation with:

- manual outer CH347 CS control
- inner Bacon control words packed into `optionByte0/1/2`
- separate GBA and GBC transport contracts
- mission-specific workflow logic above transport

The most important behavioral pattern is:

- GBC stability comes from a dedicated optimized transport wrapper
- GBA correctness depends on reproducing word-address command sequences and bus hold/release behavior exactly

So if later the ESP side is aligned to Bacon, the right target is not "make GBA look like GBC".
The right target is:

- keep the same layering that GBC already has
- then encode the **GBA host transport** with the same level of discipline and fidelity
