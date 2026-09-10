# AG32 MCU SPI protocol v1

The AG32 board boots in the existing Bacon legacy mode. The legacy CS0, CS1,
and dual-CS encodings remain unchanged. MCU mode is entered only by clocking
the eight ASCII bytes `MORI2MCU` while both chip-select signals are high.
`MORI2LEG` returns to legacy mode in hardware, even if the MCU is not running.

In MCU mode the chip-select combinations are reassigned:

| CS1 | CS0 | Operation |
| --- | --- | --- |
| 1 | 0 | Request frame, ESP32 to AG32 SRAM |
| 0 | 1 | Response frame, AG32 SRAM to ESP32 |
| 0 | 0 | 32-bit transport status |
| 1 | 1 | Hardware mode-control magic |

Control requests and responses start with a 32-byte little-endian header. Payloads
are CRC32 protected and the on-wire transfer is padded to a 32-bit boundary.
The maximum request or response payload is 8192 bytes. A request is complete
only after CS0 rises and every received word has reached AG32 SRAM. A response
becomes visible only after the MCU publishes its address and exact word count.
The published response buffer starts with one zero dummy word followed by the
32-byte response header. ESP32 clocks and discards that dummy word; it is part
of the published TX word count and makes the first real header word stable
before the response transaction begins. Readiness now also requires the first
two FIFO words to be fetched; an arbitrary host pre-read delay is unnecessary.
The host may clock up to the response capacity declared in its request. Bytes
after the response's padded payload are ignored; the header payload length and
CRC still cover only the actual response.

The protocol uses request sequence numbers. A response must repeat both the
request sequence and opcode. ESP32 rejects mismatches instead of retrying the
operation through legacy mode. `auto` mode may select legacy only when the
initial MCU capability handshake is unavailable; the active mode is always
reported.

Command values `A0`, `A2`, and `F0` through `FC` retain the Beggar programmer's
operation meanings. Their transport framing, CRC enforcement, address flags,
and error responses are new and are not wire-compatible with its USB CDC
packet format. `PING` reports protocol version/capabilities. `RAW_BACON_EXEC`
is the compatibility command for operations not yet promoted to a high-level
MCU command (reserved, not implemented). The currently implemented cartridge
commands are power A0, read/write F5/F6/F7/F8/FA/FB, and standard AMD buffered
program F4/FC. Other declared opcodes are not advertised as working commands.

## Continuous transfers

Capability bit 7 enables `STREAM_BEGIN` (0x20). Its 12-byte payload is:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 1 | Beggar operation opcode (or diagnostic ECHO 0x02) |
| 1 | 1 | Reserved, zero |
| 2 | 2 | NOR programming buffer bytes, little endian |
| 4 | 4 | Initial bus address, little endian |
| 8 | 4 | Total data bytes, little endian |

The control acknowledgement returns 4096 as the block size. Address and total
size are sent once. Each following block contains min(4096, remaining) bytes;
the last block is zero-padded to four bytes on the wire. GBA ROM addresses are
word addresses, GBC/RAM addresses are byte addresses. A stream cannot cross
the GBA 32 MiB bus window or the GB 16-bit bus address boundary. Physical bank
and MBC selection remains explicit outside that stream.

- Writes: CS0 carries data, alignment padding, then CRC32 of unpadded data.
  No per-block address, length, opcode or sequence header is sent.
- Reads: AG32 prepares the next block after the previous block is consumed.
  ESP32 polls readiness, then reads through CS1; it sends no new read command.
- Each response has one dummy word, a 32-bit status, optional padded data,
  and CRC32 covering status plus padded data. Responses to writes contain no
  data. Read/echo responses retain their expected length even on an error.
- A program acknowledgement means the NOR algorithm has completed its ready
  poll. Receiving bytes into SRAM is not acknowledged as a completed burn.
- Both peers advance the cursor by the completed block. A failed block ends
  the stream; no automatic replay or legacy write retry is performed.
- `MORI2LEG` aborts a stream and releases transport state. The MCU re-arms its
  control buffer. CS deassertion ends a DMA block, not the logical stream.

Transport status bytes are A7 32 01 flags. Flag bits 0/1/2/3 indicate MCU mode,
response ready, request unavailable, and error. Send a request only with bit 2
clear. This includes the gap while the MCU changes from control to raw data.
CS lines are separate GPIO writes: a clockless intermediate selection must
not be interpreted as an empty request or an interrupted response.

The SPI transport uses native rising-edge MOSI capture and falling-edge MISO
output. A two-word FIFO separates AHB fetches from SPI output; status polls
do not consume that FIFO. Legacy Bacon command decoding is preserved, but
the wrapper/mux routing changes still require actual legacy regression.
