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

Every request and response starts with a 32-byte little-endian header. Payloads
are CRC32 protected and the on-wire transfer is padded to a 32-bit boundary.
The maximum request or response payload is 8192 bytes. A request is complete
only after CS0 rises and every received word has reached AG32 SRAM. A response
becomes visible only after the MCU publishes its address and exact word count.
The published response buffer starts with one zero dummy word followed by the
32-byte response header. ESP32 clocks and discards that dummy word; it is part
of the published TX word count and makes the first real header word stable
before the response transaction begins.
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
MCU command.
