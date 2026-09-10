#pragma once
#include <stdint.h>
#include <assert.h>
typedef uint32_t StackType_t;
typedef int StaticTask_t;
typedef int StaticSemaphore_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define configASSERT(x) assert(x)
