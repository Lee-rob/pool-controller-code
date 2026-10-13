#ifndef FREERTOS_TASK_H
#define FREERTOS_TASK_H

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

extern BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                              uint32_t stack_depth, void *arg,
                              unsigned priority, TaskHandle_t *handle);
extern void vTaskDelay(TickType_t ticks);
extern void vTaskDelete(TaskHandle_t task);

#endif
