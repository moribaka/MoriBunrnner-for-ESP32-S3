#pragma once
#include <stdbool.h>
static inline bool esp_ptr_dma_capable(const void *p) { (void)p; return false; }
