#pragma once
#include <stdlib.h>
#include <stdbool.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
extern bool fail_image_alloc;
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return fail_image_alloc ? NULL : malloc(n); }
