# Lyra audio port

Source: https://github.com/coderkei/lyra-firmware
Revision: f992c2a527864d60209947cd584174de52f05183
Copyright 2026 Emotivate Lyra contributors. Apache-2.0; see LICENSE.

`lyra_audio.cpp` and `lyra_audio.h` are adapted from that revision. Changes:
single MoriBurnner I2S amplifier output (GPIO 15/16/7), mono mix at output,
4 x 512-frame DMA ring, 16/4 KiB worker stacks, PSRAM CPU buffers, existing
system.ini volume persistence, deferred byte-position resume, AAC M4A sample
table playback from offset zero, and a DMA silence fence at normal EOF.
The board and storage adapter files are MoriBurnner integration code.

The original 320x480 touch UI, board peripherals and graphics are not included.
The existing MoriBurnner button UI and music_player C API use this backend.
Read-ahead and PCM rings remain 256 KiB each. The physical output is 16-bit
mono duplicated into stereo I2S slots, with upstream digital gain headroom;
support for lossless input formats is not a claim of bit-perfect 24-bit output.

The decoder dependency remains the project's installed Espressif esp_audio_codec
2.5.0 (Espressif Modified MIT, use with Espressif products). Upstream currently
locks 2.6.2. This port uses ESP-IDF 5.5.1 rather than upstream's 6.0.2.
No upstream display/PNG/JPEG dependencies are imported.
