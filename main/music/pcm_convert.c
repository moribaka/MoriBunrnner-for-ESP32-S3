#include "pcm_convert.h"
#include <string.h>

void music_pcm_apply_volume_stereo16(uint8_t *buf, size_t len, uint8_t volume_percent)
{
    int16_t *samples = (int16_t *)buf;
    size_t sample_count = len / sizeof(int16_t);

    if (buf == NULL || len == 0U || volume_percent >= 100U) {
        return;
    }
    if (volume_percent == 0U) {
        memset(buf, 0, len);
        return;
    }
    for (size_t i = 0; i < sample_count; ++i) {
        samples[i] = (int16_t)(((int32_t)samples[i] * (int32_t)volume_percent) / 100);
    }
}

static int16_t music_player_read_sample_as_s16(const uint8_t *src, uint8_t bits)
{
    switch (bits) {
        case 8: return (int16_t)(((int32_t)src[0] - 128) * 256);
        case 16: return (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
        case 24: return (int16_t)((uint16_t)src[1] | ((uint16_t)src[2] << 8));
        case 32: return (int16_t)((uint16_t)src[2] | ((uint16_t)src[3] << 8));
        default: return 0;
    }
}

size_t music_pcm_convert_frame_to_stereo16(
    const uint8_t *src,
    size_t src_size,
    uint8_t bits_per_sample,
    uint8_t channels,
    uint8_t *dst,
    size_t dst_size)
{
    size_t bytes_per_sample = (size_t)((bits_per_sample + 7U) / 8U);
    size_t frame_bytes;
    size_t frame_count;
    int16_t *out = (int16_t *)dst;

    if (src == NULL || dst == NULL || channels == 0U || bytes_per_sample == 0U) {
        return 0U;
    }
    frame_bytes = bytes_per_sample * channels;
    if (frame_bytes == 0U) {
        return 0U;
    }
    frame_count = src_size / frame_bytes;
    if (frame_count > dst_size / (sizeof(int16_t) * 2U)) {
        frame_count = dst_size / (sizeof(int16_t) * 2U);
    }

    if (bits_per_sample == 16U && channels == 1U) {
        for (size_t i = 0; i < frame_count; ++i) {
            const uint8_t *frame = src + (i * 2U);
            int16_t sample = (int16_t)((int16_t)frame[0] | ((int16_t)frame[1] << 8));

            out[i * 2U] = sample;
            out[i * 2U + 1U] = sample;
        }
        return frame_count * sizeof(int16_t) * 2U;
    }

    if (bits_per_sample == 16U && channels == 2U) {
        for (size_t i = 0; i < frame_count; ++i) {
            const uint8_t *frame = src + (i * 4U);
            int32_t left = (int16_t)((int16_t)frame[0] | ((int16_t)frame[1] << 8));
            int32_t right = (int16_t)((int16_t)frame[2] | ((int16_t)frame[3] << 8));
            int16_t sample = (int16_t)((left + right) / 2);

            out[i * 2U] = sample;
            out[i * 2U + 1U] = sample;
        }
        return frame_count * sizeof(int16_t) * 2U;
    }

    for (size_t i = 0; i < frame_count; ++i) {
        const uint8_t *frame = src + (i * frame_bytes);
        int32_t sample = 0;

        if (channels == 1U) {
            sample = music_player_read_sample_as_s16(frame, bits_per_sample);
        } else {
            int32_t left = music_player_read_sample_as_s16(frame, bits_per_sample);
            int32_t right = music_player_read_sample_as_s16(frame + bytes_per_sample, bits_per_sample);
            sample = (left + right) / 2;
        }
        out[i * 2U] = (int16_t)sample;
        out[i * 2U + 1U] = (int16_t)sample;
    }

    return frame_count * sizeof(int16_t) * 2U;
}

