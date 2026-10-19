#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/heater_freshness.h"
#include "../main/pool_state.h"
#include "freertos/task.h"

pool_state_t s_pool_state;
SemaphoreHandle_t s_pool_state_mutex = (void *)1;
static TaskFunction_t worker;
static void *worker_arg;
static void *allocated;
static size_t allocation_size;
static bool allocation_fails, task_fails, lock_fails;
static unsigned creates, frees, publishes, delays;
static uint32_t ticks;
static bool locked;
static jmp_buf stop_worker;

void *__real_malloc(size_t size);
void __real_free(void *pointer);
void *__wrap_malloc(size_t size)
{
    allocation_size = size;
    if (allocation_fails) return NULL;
    allocated = __real_malloc(size);
    return allocated;
}
void __wrap_free(void *pointer)
{
    frees++;
    __real_free(pointer);
    if (pointer == allocated) allocated = NULL;
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *handle)
{
    (void)name; (void)priority; (void)handle;
    creates++;
    assert(stack == 3072);
    worker = task;
    worker_arg = arg;
    return task_fails ? pdFALSE : pdPASS;
}
uint32_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delay)
{
    assert(delay == pdMS_TO_TICKS(10000));
    if (++delays > 2) longjmp(stop_worker, 1);
}
void vTaskDelete(TaskHandle_t task) { (void)task; assert(false); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait)
{
    (void)mutex; (void)wait;
    if (lock_fails) return pdFALSE;
    assert(!locked);
    locked = true;
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t mutex)
{
    (void)mutex;
    assert(locked);
    locked = false;
}
void mqtt_publish_heater_telemetry(const pool_state_t *snapshot, int index, bool force)
{
    publishes++;
    assert(snapshot == allocated); // Publishing must use the heap-owned copy.
    assert(!locked);               // Publishing must occur after unlocking.
    assert(index == 0 && force);
    assert(!snapshot->heaters[0].communications_online);
    assert(!snapshot->heaters[0].gas_heater_valid);
    assert(!snapshot->seen_devices[0].temp1_valid);
    assert(snapshot->seen_devices[1].temp1_valid);
}

static void setup(void)
{
    assert(!allocated);
    memset(&s_pool_state, 0, sizeof(s_pool_state));
    allocation_fails = task_fails = lock_fails = locked = false;
    creates = frees = publishes = delays = 0;
    worker = NULL; worker_arg = NULL;
    ticks = 200000;
    s_pool_state_mutex = (void *)1;
    s_pool_state.heaters[0].communications_online = true;
    s_pool_state.heaters[0].gas_heater_valid = true;
    s_pool_state.heaters[0].device_last_response_ms = 1;
    s_pool_state.num_seen_devices = 2;
    s_pool_state.seen_devices[0].addr_lo = 0x71;
    s_pool_state.seen_devices[0].temp1_valid = true;
    s_pool_state.seen_devices[1].addr_lo = 0x84;
    s_pool_state.seen_devices[1].temp1_valid = true;
}

static void run_worker(void)
{
    heater_freshness_start();
    assert(creates == 1 && allocated && worker_arg == allocated);
    assert(allocation_size == sizeof(pool_state_t));
    if (!setjmp(stop_worker)) worker(worker_arg);
    assert(!locked);
    free(allocated);
}

int main(void)
{
    // The regression is exercised with a state object larger than the worker
    // stack, including controls12's complete chlorinator diagnostic cache.
    assert(sizeof(pool_state_t) > 3072);
    setup();
    run_worker();
    assert(publishes == 1 && frees == 1); // no repeated stale notifications
    puts("PASS: oversized state snapshot is heap-owned; expiry publishes once after unlocking");

    setup(); allocation_fails = true;
    heater_freshness_start();
    assert(creates == 0 && !allocated && frees == 0);
    puts("PASS: allocation failure does not start a worker with an invalid snapshot");

    setup(); task_fails = true;
    heater_freshness_start();
    assert(creates == 1 && !allocated && frees == 1);
    puts("PASS: task creation failure releases the snapshot");

    setup(); lock_fails = true;
    run_worker();
    assert(publishes == 0 && s_pool_state.heaters[0].communications_online);
    puts("PASS: mutex timeout does not publish uninitialised data");

    setup(); s_pool_state.heaters[0].device_last_response_ms = ticks - 180000;
    run_worker();
    assert(publishes == 0 && s_pool_state.heaters[0].communications_online);
    puts("PASS: telemetry at the timeout boundary remains online");

    setup(); s_pool_state.heaters[0].device_last_response_ms = 0;
    run_worker();
    assert(publishes == 0);
    puts("PASS: an absent response timestamp is not fabricated into an expiry");

    setup(); ticks = 100; s_pool_state.heaters[0].device_last_response_ms = UINT32_MAX - 180000;
    run_worker();
    assert(publishes == 1);
    puts("PASS: expiry remains correct across uptime wrap");
    return 0;
}
