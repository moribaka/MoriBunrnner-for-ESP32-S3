#pragma once
#include "FreeRTOS.h"
extern unsigned static_task_creations, notifications;
static inline TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void *), const char *name,
    unsigned size, void *arg, unsigned priority, StackType_t *stack, StaticTask_t *tcb, int core)
{ (void)fn; (void)name; (void)size; (void)arg; (void)priority; (void)stack; (void)core; ++static_task_creations; return tcb; }
static inline void xTaskNotifyGive(TaskHandle_t task) { (void)task; ++notifications; }
static inline unsigned ulTaskNotifyTake(int clear, unsigned ticks) { (void)clear; (void)ticks; return 1; }
static inline unsigned uxTaskGetStackHighWaterMark2(TaskHandle_t task) { (void)task; return 4096; }
static inline void vTaskDelay(unsigned ticks) { (void)ticks; }
