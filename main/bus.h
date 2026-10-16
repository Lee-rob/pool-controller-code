#ifndef BUS_H
#define BUS_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t rx_bytes;
    uint32_t last_rx_ms;
    uint32_t tx_attempts;
    uint32_t tx_successes;
    uint32_t tx_skipped_transmitter_busy;
    uint32_t tx_skipped_bus_active;
    uint32_t tx_failures;
    uint32_t last_tx_ms;
} bus_diagnostics_t;

// Initialise the bus UART (GPIO, driver, TX inversion).
void bus_init(void);

// Read bytes from the bus UART. Returns number of bytes read.
int bus_read(uint8_t *buffer, size_t max_len, uint32_t timeout_ms);

// Parse a hex-encoded message and send it to the bus UART.
// hex_string: space-separated or packed hex bytes,
//   e.g. "02 00 F0 FF FF 80 00 39 0E B7 08 04 0C 03"
// Returns the number of bytes sent, or -1 on parse error.
int bus_send_message(const char *hex_string);

// Send a raw byte buffer to the bus UART.
// Logs the TX bytes, waits for TX completion, and flashes the TX LED.
// Returns the number of bytes sent, or -1 on error.
int bus_send_bytes(const uint8_t *data, size_t len);

// Return a lock-free snapshot of the UART activity and transmit outcomes.
// Counters reset only when the gateway reboots.
void bus_get_diagnostics(bus_diagnostics_t *out);

#endif // BUS_H
