#pragma once
#include <stdint.h>
static inline int64_t esp_timer_get_time(void) { static int64_t now; return now += 100000; }
static inline void esp_rom_delay_us(unsigned us) { (void)us; }
