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

No cartridge erase/program test has been performed at this checkpoint.
32 MiB burn comparisons and stream board qualification remain pending.
