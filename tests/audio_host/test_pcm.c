#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/music/pcm_convert.h"

int main(void)
{
    const int16_t values[] = {-32768, -32767, -101, -100, -1, 0, 1, 99, 100, 32766, 32767};
    for (unsigned volume = 0; volume <= 100; ++volume) {
        int16_t samples[sizeof(values) / sizeof(values[0])];
        memcpy(samples, values, sizeof(values));
        music_pcm_apply_volume_stereo16((uint8_t *)samples, sizeof(samples), volume);
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
            assert(samples[i] == (int16_t)((int32_t)values[i] * (int32_t)volume / 100));
    }
    for (unsigned bits = 8; bits <= 32; bits += 8) {
        for (unsigned channels = 1; channels <= 4; ++channels) {
            uint8_t source[16 * 4 * 4] = {0};
            int16_t expected[32], actual[32];
            unsigned width = bits / 8;
            for (unsigned frame = 0; frame < 16; ++frame) {
                int32_t left = 0, right = 0;
                for (unsigned channel = 0; channel < channels; ++channel) {
                    int16_t value = values[(frame + channel) % (sizeof(values) / sizeof(values[0]))];
                    uint8_t *out = source + (frame * channels + channel) * width;
                    if (bits == 8) {
                        out[0] = (uint8_t)((frame * 17 + channel * 71) & 255);
                        value = ((int32_t)out[0] - 128) * 256;
                    } else {
                        memset(out, 0x5A, width);
                        out[width - 2] = (uint16_t)value & 255;
                        out[width - 1] = (uint16_t)value >> 8;
                    }
                    if (channel == 0) left = value;
                    if (channel == 1) right = value;
                }
                expected[frame * 2] = expected[frame * 2 + 1] = channels == 1 ? left : (left + right) / 2;
            }
            assert(music_pcm_convert_frame_to_stereo16(source, 16 * channels * width, bits, channels,
                (uint8_t *)actual, sizeof(actual)) == sizeof(actual));
            assert(memcmp(expected, actual, sizeof(actual)) == 0);
            assert(music_pcm_convert_frame_to_stereo16(source, 16 * channels * width, bits, channels,
                (uint8_t *)actual, 13) == 12);
            assert(memcmp(expected, actual, 12) == 0);
        }
    }
    assert(music_pcm_convert_frame_to_stereo16(NULL, 0, 16, 2, NULL, 0) == 0);
    puts("PCM formats, mixing, volume and output bounds passed");
    return 0;
}
