# GBA Save Rework TODO

## Goal

Implement a new GBA save import/export path modeled after `H:\dev\ChisLinkPremium-main` while keeping the existing save path intact for rollback.

The new path must be separate from the old `RAM` import/export path. We should not add silent fallback behavior. If a selected save type or transport is unsupported or fails, return a clear error instead of quietly switching logic.

## Current state

- Existing save import/export in this repo is built around the old `RAM` path:
  - `BURNER_JOB_WRITE_RAM`
  - `BURNER_JOB_READ_RAM`
  - `BURNER_JOB_VERIFY_RAM`
- That path is coupled to:
  - `burner_bacon_mbc5_ram_*`
  - `burner_spi_prepare_ram()`
  - UI options like `FRAM` and `RAM latency`
- It is suitable for the current MBC5 save flow, but not a clean fit for ChisLink-style GBA save handling.

## ChisLink reference notes

Relevant reference areas:

- `chislink-mcu/main/gba_spi_cart.cpp`
- `chislink-mcu/main/gba_spi_tasks.cpp`
- `chislink-boot/src/cl_spi_cart.cpp`
- `chislink-mcu/main/gamedb.cpp`

ChisLink splits GBA backup handling by explicit save type:

- `SRAM`
- `EEPROM`
- `FLASH`
- `BATTERYLESS SRAM`

And its MCU/boot flow keeps these operations separate:

- `read_gba_backup(size, save_type, ...)`
- `write_gba_backup(size, save_type, ...)`
- backup type analysis
- explicit size and type handling
- CRC verification per transfer chunk

## ChisLink-style flow

1. Analyze cartridge first
2. Determine GBA save type
3. Store detected type/size in current cart state
4. Show detected type in UI
5. Route import/export/verify by explicit save type

That means we should mirror the ChisLink idea of:

- detect first
- then dispatch by type

instead of treating all GBA saves as one generic RAM path.

## Implementation plan

1. Keep the old RAM path untouched.
2. Add a new GBA save type enum and store it separately from `ram_fram`.
3. Detect GBA save type during cart analysis.
4. Store detected save type and detected save size in cart probe status.
5. Show detected save type in the left cart info panel.
6. Add new task/job types for GBA save read/write/verify.
7. Add new UI actions and menu entries for the new GBA save path.
8. Add new worker entry points:
   - `burner_run_write_gba_save_job_new()`
   - `burner_run_read_gba_save_job_new()`
   - `burner_run_verify_gba_save_job_new()`
9. Add new low-level helpers:
   - `burner_bacon_gba_save_read_block_new()`
   - `burner_bacon_gba_save_write_block_new()`
10. Implement in phases:
   - phase 1: structure, enums, UI routing, task routing
   - phase 2: detect and display save type during analysis
   - phase 3: SRAM path
   - phase 4: FLASH save path
   - phase 5: EEPROM path
   - phase 6: batteryless SRAM if hardware path is confirmed

## Rules for this rework

- Do not replace the old save path.
- Do not merge the new GBA save path into the old `ram_fram` switch.
- Do not silently downgrade one save type into another.
- Do not silently clamp a requested save type into an arbitrary supported type.
- If a save type is not implemented yet, fail clearly.

## Phase 1 deliverables

- New enum for GBA save type
- New fields in task/job request structs
- New job modes for GBA save read/write/verify
- New UI labels and actions for "new" GBA save operations
- New routing from UI into the new job path
- Build must stay green

## Phase 2 target

Implement the ChisLink-style "detect first" step:

- run GBA save type detection during cart analysis
- save the result into probe status
- show save type and detected size in the UI
- if detection is unknown, show unknown clearly

## Phase 3 target

Implement real GBA SRAM save import/export in the new path with explicit size selection and no fallback to the old RAM path.

## Notes

- ChisLink itself uses backup-type analysis plus game DB fallback.
- In this repo, the first usable slice can start with ROM signature scanning and explicit unknown results.
- Do not silently convert unknown into SRAM/FLASH/EEPROM.
