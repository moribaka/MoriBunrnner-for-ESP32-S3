#pragma once
#include "pin_map.h"
namespace lyra::board::jc3248w535en {
constexpr auto kAudioI2sPort = I2S_NUM_0;
constexpr auto kAudioBclkGpio = MORI_PIN_I2S_BCK;
constexpr auto kAudioLrclkGpio = MORI_PIN_I2S_WS;
constexpr auto kAudioDataOutGpio = MORI_PIN_I2S_SD;
} // namespace lyra::board::jc3248w535en
