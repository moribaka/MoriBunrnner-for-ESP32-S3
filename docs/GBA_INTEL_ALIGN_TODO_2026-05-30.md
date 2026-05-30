## GBA Intel Alignment TODO 2026-05-30

### P0

- [x] Block Intel ROM write and chip erase when `CFI` probe is not valid.
- [x] Keep Intel geometry and buffer size sourced from valid `CFI` only.
- [x] Stop using library/default fallback geometry for Intel erase/program.
- [x] Align Intel `CFI` enter/reset flow with GBX-compatible behavior.
- [x] Use sector-boundary erase/write scheduling for all Intel write paths.
- [x] Reuse next-sector pre-erase for Intel outside pipeline-only mode.

### P1

- [x] Keep Intel buffered-program fallback, but only inside a confirmed valid Intel path.
- [x] Add GBX-style special handling path for Intel 88B0 family and log raw CFI fields.
- [x] Force Intel 88B0 family to linear GBA mapping instead of auto multicart banking.
- [ ] Tighten Intel timeout/error logs with sector, bank, status, and write address.
- [ ] Review whether Intel final reset sweep should stay card-specific only.
- [ ] Remove success-path "fallback" wording from Intel logs.

### Validation

- [ ] Re-test Intel `89 00 B0 88 04 00 89 00` cart for ID read, CFI read, first-sector erase, buffered program, full write, verify.
- [ ] Compare TE28F128/TE28F256 command flow against GBX configs.
- [ ] Capture debug logs for Intel `QRY`, primary cmdset, sector map, buffer size, and D0/D1 swap state.
