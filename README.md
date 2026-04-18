# MoriBunrnner for ESP32-S3

ESP32-S3 firmware and companion FPGA project sources for a web-based cartridge burner targeting GBA and MBC5 flash carts.

## Repository layout

- `main/`: ESP-IDF application source
- `example/`: FPGA project source files
- `tools/`: small local helper tools

## Build notes

- This is an ESP-IDF project built with `idf.py`.
- The root `CMakeLists.txt` currently references a local LVGL checkout at `../lvgl-9.4.0`.
- Generated outputs, release packages, dependency caches, and backup/reference files are intentionally excluded from this repository.
