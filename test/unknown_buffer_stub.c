#include <stdint.h>
#include "../main/unknown_buffer.h"

// Counting stub — message_decoder.c calls this but the real implementation
// uses FreeRTOS primitives not available in the host test environment.
// Tests read the counters to check a frame was (or was not) flagged.
int unknown_buffer_record_count = 0;
unknown_reason_t unknown_buffer_last_reason;

void unknown_buffer_record(const uint8_t *data, int len, unknown_reason_t reason)
{
    (void)data; (void)len;
    unknown_buffer_record_count++;
    unknown_buffer_last_reason = reason;
}
