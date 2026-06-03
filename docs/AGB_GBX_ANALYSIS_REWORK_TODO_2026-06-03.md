# AGB GBX Analysis Rework TODO 2026-06-03

## Scope

This work is limited to **AGB GBX analysis/detection only**.

Allowed:

- Adjust how AGB GBX profiles are detected and selected.
- Adjust how GBX cache/profile matching is used during AGB detection.
- Reorder when existing CHIS probe results are consumed by the GBX analysis path.
- Add AGB-only matching helpers if needed.

Not allowed:

- Do **not** change CHIS probe logic in `main/burner/core/burn/burner_gba_lowlevel.c`.
- Do **not** change CHIS erase/program behavior.
- Do **not** change GBX erase/program templates.
- Do **not** change GBX write/erase execution flow in `main/burner/core/burn/burner_gba_gbx_lowlevel.c`.
- Do **not** retune current buffered write, sector erase, or chip erase behavior.

Short version: **only fix GBX analysis; leave CHIS analysis and all erase/write paths alone.**

## Problem Summary

Current AGB GBX detection is failing to match many carts that are already present in FlashGBX config.

Main causes:

1. `burner_gba_gbx_probe_locked()` currently does:
   - CHIS ID/CFI probe first
   - then reverse-lookup a FlashGBX AGB profile from that result

2. FlashGBX original `DetectFlash()` is method-driven:
   - execute each profile's `read_identifier` method
   - read resulting ID bytes
   - then resolve profile from `method + id`

3. Current ID matching is stricter than FlashGBX:
   - if profile ID is shorter than observed ID, trailing bytes must currently be `00/FF`
   - FlashGBX effectively accepts prefix match

4. Current ambiguity handling is too aggressive:
   - multiple valid candidates often collapse into `ESP_ERR_NOT_FOUND`
   - some of these can be resolved with size/geometry/cmdset information

## Current Evidence

- FlashGBX AGB profiles scanned: `55`
- Unique AGB `read_identifier` methods: `10`
- AGB profiles using `unlock_read`: `1`
- AGB profiles using `read_identifier_at`: `0`
- Duplicate AGB flash ID groups: `8`

Implication:

- AGB is a strong candidate for **method-driven detection**
- the method set is small enough to scan on-device
- we do not need to touch write/erase paths to fix the miss rate

## Existing Code Reuse

Good news: the repo already has the right shape on the DMG/GBC side.

Useful references:

- `main/burner/core/burn/burner_gbc_flash_lowlevel.c`
  - `burner_gbc_gbx_read_id_with_profile()`
  - `burner_gbc_gbx_probe_method_visitor()`
  - `burner_gbc_gbx_probe_locked()`
- `main/burner/core/burn/burner_gbx_profile.c`
  - `burner_gbx_visit_cached_methods_by_type()`
  - `burner_gbx_find_cached_profile()`

AGB should be reworked to follow the same broad detection model, while keeping its current write/erase execution untouched.

## Target Design

### Stage 1: Method-driven AGB ID scan

Add an AGB scan path that:

1. iterates unique cached AGB `read_identifier` methods
2. runs:
   - `reset`
   - optional `unlock_read`
   - optional `unlock`
   - `read_identifier`
   - read flash ID bytes
   - `reset`
3. compares against baseline ROM bytes
4. treats "ID window changed" as a valid probe candidate

This should happen inside the AGB GBX path, not inside CHIS.

### Stage 2: Resolve profile from `method + id`

After a method returns a changed ID window:

1. look up GBX cache by:
   - type = `AGB`
   - exact `read_identifier` method
   - observed ID bytes
2. prefer **prefix match** behavior for AGB analysis
3. keep match length as an input to ranking, not as a hard tail-blank gate

Important:

- Do this as **AGB-specific behavior**
- do not globally change DMG/GBC matching rules unless later proven safe

### Stage 3: Secondary disambiguation

If more than one candidate remains, resolve with existing metadata instead of immediately failing:

- `command_set`
- `flash_size`
- `sector_size`
- sector geometry
- `d0d1` swap state
- bank-ID match if applicable

Only return `ESP_ERR_NOT_FOUND` if the result is still genuinely unresolved after secondary scoring.

### Stage 4: CHIS stays as enrichment/fallback

CHIS remains unchanged.

Recommended use:

- GBX method scan decides which profile family is the best fit
- existing CHIS probe can still provide:
  - `CFI`
  - geometry
  - `cmdset`
  - `d0d1` information
  - buffer size

But CHIS output should no longer be the hard gate for whether a GBX profile can be recognized.

## Preferred Change Boundary

Primary files allowed to change:

- `main/burner/core/burn/burner_gba_gbx_lowlevel.c`
- `main/burner/core/burn/burner_gbx_profile.c`

Try hard not to change:

- `main/burner/core/burn/burner_gba_lowlevel.c`
- any CHIS write/erase code
- any GBX program/erase template execution code

## Proposed Implementation Steps

### Phase 0: Guardrails

- [x] Write down the no-touch boundary in code comments or task notes before editing:
  - CHIS probe logic unchanged
  - CHIS write/erase unchanged
  - GBX write/erase templates unchanged

### Phase 1: Add AGB method-scan helpers

- [x] Add `burner_gba_gbx_read_chip_bytes()` helper.
- [x] Add `burner_gba_gbx_first_flash_id_len()` helper.
- [x] Add `burner_gba_gbx_read_id_with_profile()` helper modeled after GBC.
- [x] Add AGB probe context struct for method scan results.
- [x] Add `burner_gba_gbx_probe_method_visitor()` using cached AGB methods.

### Phase 2: Switch AGB GBX probe entry to method-driven detection

- [x] Rework `burner_gba_gbx_probe_locked()` to start from `burner_gbx_visit_cached_methods_by_type("AGB", ...)`.
- [x] Stop using CHIS ID bytes as the primary profile lookup key.
- [x] Keep existing CHIS probe in the AGB analysis path as state/enrichment instead of the profile lookup key.
- [x] Preserve current fallback behavior when GBX strict match fails.

### Phase 3: Introduce AGB-only prefix match behavior

- [x] Add an AGB-specific cache/profile match helper that allows prefix match without requiring trailing `00/FF`.
- [x] Avoid changing default DMG/GBC behavior; keep strict matching as the default path and enable relaxed prefix behavior only for AGB probe resolution.
- [x] Keep match length in scoring so longer IDs still win.

### Phase 4: Improve ambiguity handling

- [x] Replace early `ambiguous => not found` behavior for AGB analysis with secondary scoring.
- [x] Use `flash_size`, `sector_size`, geometry, `cmdset`, and `d0d1` to break ties.
- [x] Only fail when ties remain after all available metadata is consumed.

### Phase 5: Keep write/erase path isolated

- [x] Verify that no changes were made to:
  - `single_write`
  - `buffer_write`
  - `sector_erase`
  - `chip_erase`
  - reset/wait execution used by actual programming flows
- [x] Verify no CHIS erase/program logic changed as part of the rework.

## Validation TODO

- [x] Fix the burn-entry reboot regression caused by large temporary GBX profile objects overflowing `burn_task` stack during re-probe.
- [ ] Re-test known failing AGB carts whose IDs already exist in FlashGBX config.
- [ ] Confirm `01 00 7E 22` class carts can be recognized through GBX analysis.
- [ ] Confirm method-driven scan still recognizes special-method carts such as:
  - `GBAMP`
  - `AR_SST39VF800A`
  - Intel/Sharp variants with non-generic methods
- [ ] Confirm duplicate-ID carts prefer the correct profile when geometry differs.
- [ ] Confirm existing successful GBX AGB matches still work.
- [ ] Confirm GBC/DMG behavior is unchanged.
- [ ] Confirm write/erase regression surface is zero by checking that only analysis-path files changed.

## Nice-to-have, Not Required for Phase 1

- [ ] Add structured debug logs:
  - which method was executed
  - baseline bytes
  - observed ID bytes
  - whether ID window changed
  - why a candidate won or lost
- [ ] Add a compact "analysis reason" string for UI/log output.

## Success Criteria

This rework is successful if:

- more AGB carts match GBX profiles already present in FlashGBX config
- special `read_identifier` methods are actually exercised
- CHIS remains unchanged
- write/erase behavior remains unchanged
- GBX analysis becomes closer to original FlashGBX behavior without reopening the already-good burn path

## Current Progress

- AGB GBX probe now uses CHIS for probe state and geometry, but no longer uses CHIS ID as the primary profile lookup key.
- AGB GBX probe now scans FlashGBX `read_identifier` methods and resolves profiles from `method + observed id`.
- AGB GBX probe now uses AGB-specific relaxed prefix matching during probe resolution.
- AGB ambiguity handling now keeps the best scored candidate and only fails when ties remain after scoring.
- The burn-entry reboot regression was traced to `burn_task` stack overflow in the new AGB GBX probe path and fixed by moving large temporary `burner_gbx_profile_t` buffers off stack.
- AGB GBX matched profiles are now analysis-only for burn flows; actual erase/program/finalize stays on the existing CHIS high-speed runtime.
- Burn prepare now reuses a fresh UI AGB GBX probe result instead of immediately re-running the full GBX method scan.
- AGB write/erase execution paths were left unchanged.
- Manual syntax verification passed by compiling `main/burner/core/ws_server.c` directly with the ESP32 toolchain after bypassing missing local `ccache`.
