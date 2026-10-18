#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "main/bus.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static int tests_run;
static int tests_failed;
static uint64_t fake_now_us;
static int buffered_checks_remaining;
static bool buffered_forever;
static esp_err_t buffered_result;
static int uart_write_result;
static int uart_write_calls;

#define CHECK(condition, message) do { \
    tests_run++; \
    if (condition) printf("  PASS: %s\n", message); \
    else { printf("  FAIL: %s\n", message); tests_failed++; } \
} while (0)

static void reset_bus(void)
{
    fake_now_us = 0;
    buffered_checks_remaining = 0;
    buffered_forever = false;
    buffered_result = ESP_OK;
    uart_write_result = -2;  // sentinel: return the requested length
    uart_write_calls = 0;
    bus_init();
}

int64_t esp_timer_get_time(void) { return fake_now_us; }
uint32_t xTaskGetTickCount(void)
{
    return fake_now_us / (portTICK_PERIOD_MS * 1000U);
}
void vTaskDelay(TickType_t ticks)
{
    fake_now_us += ticks * portTICK_PERIOD_MS * 1000U;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    (void)semaphore;
    (void)timeout;
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t semaphore) { (void)semaphore; }

esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
esp_err_t gpio_set_level(int gpio_num, uint32_t level)
{
    (void)gpio_num;
    (void)level;
    return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *config)
{
    (void)uart_num; (void)config; return ESP_OK;
}
esp_err_t uart_set_pin(uart_port_t uart_num, int tx, int rx, int rts, int cts)
{
    (void)uart_num; (void)tx; (void)rx; (void)rts; (void)cts; return ESP_OK;
}
esp_err_t uart_driver_install(uart_port_t uart_num, int rx_size, int tx_size,
                              int queue_size, void *queue, int flags)
{
    (void)uart_num; (void)rx_size; (void)tx_size;
    (void)queue_size; (void)queue; (void)flags;
    return ESP_OK;
}
esp_err_t uart_set_line_inverse(uart_port_t uart_num, uint32_t mask)
{
    (void)uart_num; (void)mask; return ESP_OK;
}
esp_err_t uart_wait_tx_done(uart_port_t uart_num, uint32_t timeout)
{
    (void)uart_num; (void)timeout; return ESP_OK;
}
int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t len, uint32_t timeout)
{
    (void)uart_num; (void)timeout;
    if (len > 0) { ((uint8_t *)buf)[0] = 0x02; return 1; }
    return 0;
}
esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size)
{
    (void)uart_num;
    if (buffered_result != ESP_OK) return buffered_result;
    if (buffered_forever || buffered_checks_remaining > 0) {
        *size = 1;
        if (buffered_checks_remaining > 0) buffered_checks_remaining--;
    } else {
        *size = 0;
    }
    return ESP_OK;
}
int uart_write_bytes(uart_port_t uart_num, const char *src, size_t size)
{
    (void)uart_num; (void)src;
    uart_write_calls++;
    return uart_write_result == -2 ? (int)size : uart_write_result;
}

void led_flash_tx(void) {}

int main(void)
{
    static const uint8_t frame[] = {0x02, 0x03};

    printf("Bus arbitration tests\n");

    reset_bus();
    CHECK(bus_send_bytes(frame, sizeof(frame)) == (int)sizeof(frame),
          "an idle bus sends successfully");
    CHECK(fake_now_us >= 12000U,
          "the full 12 ms quiet guard elapses before TX");

    reset_bus();
    buffered_checks_remaining = 2;
    CHECK(bus_send_bytes(frame, sizeof(frame)) == (int)sizeof(frame),
          "a temporarily busy RX buffer is retried");
    CHECK(fake_now_us >= 24000U,
          "RX activity restarts the complete quiet guard");

    reset_bus();
    buffered_forever = true;
    CHECK(bus_send_bytes(frame, sizeof(frame)) < 0,
          "a continuously busy bus fails at the bounded deadline");
    CHECK(fake_now_us >= 250000U && fake_now_us < 270000U,
          "busy-bus waiting remains close to the 250 ms deadline");
    CHECK(uart_write_calls == 0,
          "no UART write occurs while RX remains active");

    reset_bus();
    buffered_result = ESP_FAIL;
    CHECK(bus_send_bytes(frame, sizeof(frame)) < 0,
          "failure to inspect RX is fail-safe");
    CHECK(uart_write_calls == 0,
          "RX inspection failure never writes blind");

    reset_bus();
    uart_write_result = 1;
    CHECK(bus_send_bytes(frame, sizeof(frame)) < 0,
          "a partial UART write is reported as failure");

    reset_bus();
    // Reproduce the captured diagnostic error after multiple 32-bit-us wraps.
    fake_now_us = 65700000000ULL;
    uint8_t received;
    CHECK(bus_read(&received, 1, 0) == 1, "RX is recorded after 18 hours uptime");
    bus_diagnostics_t diagnostics;
    bus_get_diagnostics(&diagnostics);
    CHECK(diagnostics.last_rx_ms == 65700000U,
          "RX diagnostic keeps the full uptime before conversion to milliseconds");
    CHECK(bus_send_bytes(frame, sizeof(frame)) == (int)sizeof(frame),
          "TX arbitration still succeeds after microsecond-clock wraps");
    bus_get_diagnostics(&diagnostics);
    CHECK(diagnostics.last_tx_ms >= 65700000U && diagnostics.last_tx_ms < 65700100U,
          "TX diagnostic uses the same millisecond epoch as the status page");

    printf("Bus arbitration summary: %d passed, %d failed\n",
           tests_run - tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
