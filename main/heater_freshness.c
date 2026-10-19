#include "heater_freshness.h"
#include "pool_state.h"
#include "mqtt_publish.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdlib.h>

#define HEATER_RESPONSE_TIMEOUT_MS 180000U
#define HEATER_FRESHNESS_POLL_MS 10000U

static const char *TAG = "HEATER_FRESHNESS";

static void freshness_task(void *arg)
{
    // This worker has a 3072-byte stack. pool_state_t can exceed that on
    // custom builds, so its snapshot is allocated once by the starter and
    // belongs to this task for its lifetime. Never put a full state here.
    pool_state_t *snapshot = arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEATER_FRESHNESS_POLL_MS));
        bool publish = false;
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (s_pool_state_mutex && xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            pool_heater_t *heater = &s_pool_state.heaters[0];
            if (heater->communications_online && heater->device_last_response_ms != 0 &&
                (uint32_t)(now - heater->device_last_response_ms) > HEATER_RESPONSE_TIMEOUT_MS) {
                heater->communications_online = false;
                heater->gas_heater_valid = false;
                for (int i = 0; i < s_pool_state.num_seen_devices; i++) {
                    if (s_pool_state.seen_devices[i].addr_hi == 0x00 &&
                        s_pool_state.seen_devices[i].addr_lo == 0x71) {
                        s_pool_state.seen_devices[i].temp1_valid = false;
                        s_pool_state.seen_devices[i].temp2_valid = false;
                    }
                }
                publish = true;
                ESP_LOGW(TAG, "Viron heater telemetry expired after %u seconds",
                         HEATER_RESPONSE_TIMEOUT_MS / 1000);
            }
            if (publish) *snapshot = s_pool_state;
            xSemaphoreGive(s_pool_state_mutex);
        }
        if (publish) mqtt_publish_heater_telemetry(snapshot, 0, true);
    }
}

void heater_freshness_start(void)
{
    pool_state_t *snapshot = malloc(sizeof(*snapshot));
    if (!snapshot) {
        ESP_LOGE(TAG, "Cannot allocate heater freshness snapshot");
        return;
    }
    if (xTaskCreate(freshness_task, "heater_freshness", 3072, snapshot, 3, NULL) != pdPASS) {
        free(snapshot);
        ESP_LOGE(TAG, "Cannot create heater freshness task");
    }
}
