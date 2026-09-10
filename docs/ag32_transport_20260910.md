# AG32 SPI bring-up, 2026-09-10

Base commit: 490b25e. ESP32-S3 MAC a4:cb:8f:f2:c4:c0, native COM26.
SPI configured and actual 40 MHz throughout; AG32 MCU/AHB 150 MHz.

## Verified observations

- ESP32 app flashing completed with esptool hash verification.
- Onboard ESP32→AG32 SWD programmed/verified all three records repeatedly.
- Initial FIFO candidate still failed PING. SWD snapshot proved the request
  buffer was zero and response was a correctly stored BAD_LENGTH frame;
  the SPI received header itself was also corrupt.
- Clockless CS skew created an empty RX request during a status poll.
  Request/response completion now requires actual SPI clocks. TX no longer
  consumes status clocks or overwrites the last bit of a word.
- Native SPI capture/output candidate batch SHA256
  8A8C2CC623179D6E299BDE1D2ADAFF74F0325A0CA7C66CEC0A8CB8ACEAE0A1AE
  passed PING, capability 0x7e, identity 0x3155434d.
- Its ECHO test passed 100/100 transactions, 1–8192 bytes, 131590 bytes in
  each direction, 461.36 ms. See ag32_native_cs_0910_serial.log.
- Real GBA bus reads returned CFI QRY and ID 01 00 7E 22 28 22 01 22:
  S29GL01GP/GS, 134217728 bytes, 131072-byte sectors, 512-byte program buffer.
- HTTP cartridge analysis then crashed in the task scheduler with a corrupt
  backtrace. The probe wrapper and nested analysis each put a task parameter
  containing a ~8 KiB patch plan on the 16 KiB HTTP stack. Patch plans now
  have explicit burn-task heap ownership; probe parameters remain below 1 KiB.
  Real-board regression of this fix is pending at this checkpoint.

## Current stream candidate

Batch SHA256 B422A36349E1257C291FD4770D26280EACAC6D35FEF62A7D541C82BDEB722A77.
Three records, payload 107816 bytes; MCU 7744 bytes, CPLD 99944 bytes, options128.
Local batch parser compared the two payloads byte-for-byte with build outputs.
Raw streams carry address/total once and data+CRC blocks thereafter; see
shared/AG32_MCU_PROTOCOL.md. Writes acknowledge NOR-ready completion.

Full flow ran prelogic, MCU release, quartus_sh -t af_quartus.tcl (including
af_ip.tcl), Supra MODE QUARTUS/FLOW ALL/seed42/timing_more/highest, buildbatch.
Final route: 1370/2112 logic, 108/132 tiles, 1251 LUTs, 876 registers,
0/4 BRAM, 1/1 PLL, 44/128 pins. Setup -1.200 ns, user hold +0.603 ns.
Auto hold -2.714 ns is CS1→legacy oscillator clock-enable; CS clocks have
auto-derived constraints, not physical 40 MHz board qualification. Coverage
61.3%. This is hardware-test-only, not a timing-qualified release.

## Regression performed

Icarus transport: 40 MHz, AHB address/data phases plus three wait states,
separate CS GPIO transitions, status polls between publication/read,
partial response restart, partial RX rejection, arbitrary 8192-byte FIFO wrap.
Mode guard and basic legacy power compatibility tests pass. Those do not
substitute for cartridge read/erase/program regression.
Shared protocol host tests, batch payload comparison and patch host tests pass.

## Registered output candidate and read qualification

The first stream candidate exposed route-dependent MISO corruption again.
SWD confirmed a valid request and response in SRAM. Replaced output selection
with a native falling-edge shift register; the mandatory dummy word provides
synchronous preload. Captured AHB register address before decoding it to
remove the >20% setup deficit on MCU address→register-select. Declared the
external SCK pin as input (it was never driven), so the real 25 ns constraint
now reaches Supra rather than an unused inout alias.

Current batch SHA256
9987DA3E97700C3DD531B1FEA90E704492E8BA00CEB994FF97BE8CEAB2521CDD.
Programmed/verified 107816 bytes through onboard SWD. Route: 1392/2112 logic,
98/132 tiles, 1240 LUTs, 939 registers, 0 BRAM, 1 PLL, 44 pins.
SPI setup +8.394 ns / hold +0.603 ns; system setup -0.609 ns / hold +0.377 ns.
Auto hold -1.426 ns on the legacy CS path remains; board testing is separate
from complete external I/O timing qualification. User constraint coverage83%.

- PING passed, capabilities0xfe. Normal echo100/100, 131590 bytes/direction,
  472.421 ms. Raw-stream echo100/100, same data volume, 532.607 ms.
- HTTP legacy cartridge ID/CFI plus ROM analysis passed after removing the
  large embedded patch plan; no reboot.
- Legacy dumped first32MiB to `/sdcard/ROM_OUTPUT/ag32_baseline32_0910.gba`.
  Device time112164ms, bus reading26615ms, TF writing84673ms.
- MCU stream compared all33554432 bytes with that legacy dump successfully,
  including the final byte. Device time53393ms. Full status is preserved in
  `ag32_benchmark_0910.jsonl`.
- Legacy on this same final routing also verified all32MiB against the
  baseline successfully, device time33918ms. At this checkpoint MCU read
  verification is slower (53.4s vs33.9s); do not claim speedup from protocol
  overhead reduction alone.

32MiB erase/program comparisons remain pending. The test ROM Mother3 was
analyzed: FLASH512, five SRAM patch operations, output remains33554432 bytes.
