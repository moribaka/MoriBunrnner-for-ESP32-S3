#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#ifndef heap_caps_malloc
#define heap_caps_malloc(size, caps) malloc(size)
#endif
#define heap_caps_free(ptr) free(ptr)
