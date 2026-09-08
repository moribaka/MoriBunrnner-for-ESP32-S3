#pragma once
typedef void *TaskHandle_t;
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
static inline void vTaskDelay(unsigned ticks) { (void)ticks; }
