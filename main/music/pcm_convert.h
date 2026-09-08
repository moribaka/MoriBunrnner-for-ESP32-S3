#pragma once
#include <stddef.h>
#include <stdint.h>

/* Preserve the player's mono mix in both output channels. Volume is applied
 * once by the output task, after samples leave the PCM ring. */
size_t music_pcm_convert_frame_to_stereo16(const uint8_t *src, size_t src_size,
    uint8_t bits_per_sample, uint8_t channels, uint8_t *dst, size_t dst_size);
void music_pcm_apply_volume_stereo16(uint8_t *buf, size_t len, uint8_t volume_percent);
