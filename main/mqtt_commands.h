#ifndef MQTT_COMMANDS_H
#define MQTT_COMMANDS_H

#include <stdint.h>
#include "pool_state.h"

// Handle incoming MQTT command
// This function parses the topic and payload and sends appropriate UART commands
void mqtt_handle_command(const char *topic, int topic_len, const char *data, int data_len);

// Repair an observed unsafe return-valve state. When the primary pool return
// is closed in Spa mode, at least one auxiliary spa return path must be active.
void mqtt_commands_enforce_spa_valve_safety(const pool_state_t *state);

#endif // MQTT_COMMANDS_H
