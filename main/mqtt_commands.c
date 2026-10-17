#include "mqtt_commands.h"
#include "config.h"
#include "message_decoder.h"
#include "mqtt_poolclient.h"
#include "mqtt_publish.h"
#include "pool_state.h"
#include "bus.h"
#include "register_requester.h"
#include "channel_power.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>

static const char *TAG = "MQTT_COMMANDS";

typedef struct {
    uint8_t channel_id;
    uint8_t target_state;
    uint32_t sequence;
} channel_target_task_args_t;

typedef enum {
    SPA_JETS_OFF,
    SPA_JETS_LOW,
    SPA_JETS_HIGH,
    SPA_ENTER_MODE,
    SPA_RAW_VALVE_CLOSE,
} spa_transition_t;

typedef struct {
    spa_transition_t transition;
    uint8_t valve_num;
    uint8_t valve_state;
    uint32_t sequence;
} spa_transition_task_args_t;

static uint32_t s_channel_target_sequence[MAX_CHANNELS];
static uint32_t s_spa_transition_sequence;
static uint32_t s_spa_transition_running;

// ======================================================
// UART Command Helpers
// ======================================================

// Send raw UART message to pool bus
static bool send_uart_command(const uint8_t *data, size_t len)
{
    if (bus_send_bytes(data, len) < 0) {
        ESP_LOGE(TAG, "Failed to send UART command");
        return false;
    }
    return true;
}

// ======================================================
// Channel Control
// ======================================================

static void send_channel_toggle(int channel_id)
{
    uint8_t channel_idx = channel_id - 1;

    if (channel_idx >= MAX_CHANNELS) {
        ESP_LOGE(TAG, "Channel %d out of range", channel_id);
        return;
    }

    // Build toggle command
    // Pattern: 02 00 F0 FF FF 80 00 10 0D 8D [CHANNEL_IDX] [CHECKSUM] 03
    // Checksum = channel_idx (only data byte)
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x10, 0x0D, 0x8D, // Command pattern
        channel_idx,       // Channel index (0-based)
        channel_idx,       // Checksum (= channel index)
        0x03               // END
    };

    ESP_LOGI(TAG, "Toggling channel %d", channel_id);
    send_uart_command(cmd, sizeof(cmd));
}

static void handle_channel_command(int channel_id, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Channel %d command: %.*s", channel_id, payload_len, payload);
    send_channel_toggle(channel_id);
}

static bool get_channel_state(uint8_t channel_id, uint8_t *state, channel_type_t *type)
{
    if (!s_pool_state_mutex || channel_id < 1 || channel_id > MAX_CHANNELS) return false;
    if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
    const channel_state_t *channel = &s_pool_state.channels[channel_id - 1];
    bool valid = channel->configured && channel->state < CHANNEL_STATE_COUNT;
    if (valid) {
        *state = channel->state;
        if (type) *type = channel->type;
    }
    xSemaphoreGive(s_pool_state_mutex);
    return valid;
}

// Solar and Blower do not reliably accept the direct pump-mode command. Walk
// their normal Off/Auto/On toggle ring, but verify every reported state before
// sending the next toggle. A sequence number cancels an older worker if HA
// sends a newer choice while the first request is still settling.
static void channel_target_task(void *arg)
{
    channel_target_task_args_t request = *(channel_target_task_args_t *)arg;
    free(arg);
    const uint8_t idx = request.channel_id - 1;

    for (int toggle_count = 0; toggle_count < 3; toggle_count++) {
        if (__atomic_load_n(&s_channel_target_sequence[idx], __ATOMIC_RELAXED) != request.sequence) break;

        uint8_t current;
        if (get_channel_state(request.channel_id, &current, NULL) && current == request.target_state) {
            ESP_LOGI(TAG, "Channel %u reached requested state %s",
                     request.channel_id, CHANNEL_STATE_NAMES[request.target_state]);
            vTaskDelete(NULL);
            return;
        }

        send_channel_toggle(request.channel_id);

        // Wait up to 1.2 s for a controller broadcast confirming the result.
        for (int poll = 0; poll < 12; poll++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (__atomic_load_n(&s_channel_target_sequence[idx], __ATOMIC_RELAXED) != request.sequence) {
                vTaskDelete(NULL);
                return;
            }
            if (get_channel_state(request.channel_id, &current, NULL) && current == request.target_state) {
                ESP_LOGI(TAG, "Channel %u reached requested state %s",
                         request.channel_id, CHANNEL_STATE_NAMES[request.target_state]);
                vTaskDelete(NULL);
                return;
            }
        }
    }

    ESP_LOGW(TAG, "Channel %u did not reach requested state %s after three verified toggles",
             request.channel_id, CHANNEL_STATE_NAMES[request.target_state]);
    vTaskDelete(NULL);
}

static void start_toggle_to_target(int channel_id, uint8_t target_state)
{
    uint8_t current;
    if (get_channel_state((uint8_t)channel_id, &current, NULL) && current == target_state) {
        ESP_LOGI(TAG, "Channel %d is already %s", channel_id, CHANNEL_STATE_NAMES[target_state]);
        return;
    }

    channel_target_task_args_t *args = malloc(sizeof(*args));
    if (!args) {
        ESP_LOGE(TAG, "No memory for channel target worker");
        return;
    }
    args->channel_id = (uint8_t)channel_id;
    args->target_state = target_state;
    args->sequence = __atomic_add_fetch(&s_channel_target_sequence[channel_id - 1], 1, __ATOMIC_RELAXED);

    if (xTaskCreate(channel_target_task, "channel_target", 3072, args, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start channel target worker");
        free(args);
    }
}

// Set a channel directly to a named state via CMD 0x0F, rather than stepping
// it round the toggle ring. Only pump-driven channels act on this; see
// PROTOCOL.md command `0x0F`.
//
// The payload is one of the option strings published in discovery, which are
// the CHANNEL_STATE_NAMES entries verbatim, so the state code is that name's
// index. Nothing is published optimistically: the Touchscreen broadcasts the
// resulting state via CMD 0x0B and that is what updates HA, so a command it
// declines to act on simply leaves the entity where it was.
static void handle_channel_mode_command(int channel_id, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Channel %d mode command: %.*s", channel_id, payload_len, payload);

    int state = -1;
    for (int i = 0; i < CHANNEL_STATE_COUNT; i++) {
        if ((int)strlen(CHANNEL_STATE_NAMES[i]) == payload_len &&
            strncmp(payload, CHANNEL_STATE_NAMES[i], payload_len) == 0) {
            state = i;
            break;
        }
    }
    if (state < 0) {
        ESP_LOGE(TAG, "Unknown channel mode: %.*s", payload_len, payload);
        return;
    }

    channel_type_t channel_type = CHANNEL_TYPE_UNUSED;
    uint8_t current_state;
    bool have_channel = get_channel_state((uint8_t)channel_id, &current_state, &channel_type);
    if (have_channel && (channel_type == CHANNEL_TYPE_SOLAR ||
                         channel_type == CHANNEL_TYPE_SOLAR_CONTROLLER ||
                         channel_type == CHANNEL_TYPE_BLOWER)) {
        if (state > 2) {
            ESP_LOGE(TAG, "Channel %d only supports Off/Auto/On", channel_id);
            return;
        }
        ESP_LOGI(TAG, "Selecting channel %d %s via verified toggle sequence",
                 channel_id, CHANNEL_STATE_NAMES[state]);
        start_toggle_to_target(channel_id, (uint8_t)state);
        return;
    }

    // Pattern: 02 [SRC_HI] [SRC_LO] 00 50 80 00 0F 0E [HDR_CK] [CHANNEL] [STATE] [DATA_CK] 03
    // Channel is 1-based here, unlike the toggle command's 0-based index.
    uint8_t cmd[] = {
        0x02,             // START
        SELF_DEVICE_ID_HI, SELF_DEVICE_ID_LO,   // SOURCE: this firmware (Pool Controller ESP32)
        0x00, 0x50,       // DEST: Touchscreen
        0x80, 0x00,       // CONTROL
        0x0F, 0x0E,       // CMD, LEN
        0x00,             // HDR checksum, filled in below
        (uint8_t)channel_id,
        (uint8_t)state,
        (uint8_t)(channel_id + state),  // Data checksum
        0x03              // END
    };
    uint8_t hdr_ck = 0;
    for (int i = 0; i < 9; i++) hdr_ck += cmd[i];
    cmd[9] = hdr_ck;

    ESP_LOGI(TAG, "Setting channel %d to %s (0x%02X)", channel_id, CHANNEL_STATE_NAMES[state], state);
    send_uart_command(cmd, sizeof(cmd));
}

// Parse a wattage payload (a plain decimal integer, as HA's number entity
// sends). Returns false and logs on anything unparseable or out of range.
static bool parse_watts_payload(const char *payload, int payload_len, uint16_t *out_watts)
{
    char watts_str[16];
    if (payload_len <= 0 || payload_len >= (int)sizeof(watts_str)) {
        ESP_LOGE(TAG, "Power payload invalid length");
        return false;
    }
    memcpy(watts_str, payload, payload_len);
    watts_str[payload_len] = '\0';

    char *endptr;
    long watts = strtol(watts_str, &endptr, 10);
    if (endptr == watts_str || *endptr != '\0' || watts < 0 || watts > UINT16_MAX) {
        ESP_LOGE(TAG, "Invalid power value: \"%s\"", watts_str);
        return false;
    }

    *out_watts = (uint16_t)watts;
    return true;
}

// Set a channel's configured power estimate (Watts). This is local-only
// config (not a bus register), so there's no read-back — persist to NVS and
// republish the channel's state topic immediately. A value of 0 clears the
// manual estimate (e.g. once a channel's device starts reporting its own
// real power); see channel_power_get_effective.
static void handle_channel_power_command(int channel_id, const char *payload, int payload_len)
{
    uint16_t watts;
    if (!parse_watts_payload(payload, payload_len, &watts)) {
        return;
    }

    if (channel_power_set_configured((uint8_t)channel_id, watts) != ESP_OK) {
        return;
    }

    // Pass the global state directly rather than copying it onto the stack:
    // this handler runs on the default system event loop task, which this
    // project sizes at only 2304 bytes (CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE)
    // — nowhere near enough for a ~3KB pool_state_t local. mqtt_publish_channel
    // is synchronous and non-blocking (JSON build + MQTT enqueue), so holding
    // the mutex for its duration is brief and safe.
    if (s_pool_state_mutex && xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        mqtt_publish_channel(&s_pool_state, (uint8_t)channel_id);
        xSemaphoreGive(s_pool_state_mutex);
    }
}

// Set the system baseline wattage — the idle draw not attributable to any
// channel. Local-only config like the per-channel estimate, so no bus write
// and no read-back; persist and republish immediately. Needs no pool state,
// hence no mutex.
static void handle_system_power_command(const char *payload, int payload_len)
{
    uint16_t watts;
    if (!parse_watts_payload(payload, payload_len, &watts)) {
        return;
    }

    if (channel_power_set_system(watts) != ESP_OK) {
        return;
    }

    mqtt_publish_system_power();
}

// ======================================================
// Light Control
// ======================================================

static void handle_light_command(int zone, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Light zone %d command: %.*s", zone, payload_len, payload);

    // Determine state: OFF=0x00, AUTO=0x01, ON=0x02
    uint8_t state;
    if (strncmp(payload, "ON", payload_len) == 0) {
        state = 0x02;
    } else if (strncmp(payload, "OFF", payload_len) == 0) {
        state = 0x00;
    } else if (strncmp(payload, "AUTO", payload_len) == 0) {
        state = 0x01;
    } else {
        ESP_LOGE(TAG, "Invalid light command: %.*s (expected ON/OFF/AUTO)", payload_len, payload);
        return;
    }

    uint8_t reg_id = REG_ID_LIGHT_ZONE_STATE_0 + (zone - 1);

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 3A 0F B9 [REG_ID] 01 [STATE] [CHECKSUM] 03
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x3A, 0x0F, 0xB9, // Command pattern
        reg_id,     // Register ID (light zone)
        0x01,       // Slot ID (state)
        state,      // State value (OFF/AUTO/ON)
        0x00,       // Checksum (calculated below)
        0x03        // END
    };

    // Calculate checksum (sum of bytes 10-12)
    cmd[13] = (reg_id + 0x01 + state) & 0xFF;

    ESP_LOGI(TAG, "Sending light zone %d %s command", zone,
             state == 0x02 ? "ON" : (state == 0x00 ? "OFF" : "AUTO"));
    send_uart_command(cmd, sizeof(cmd));
}

static void handle_light_color_command(int zone, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Light zone %d color command: %.*s", zone, payload_len, payload);

    // Map the color name (an HA effect_list entry) back to its code in the
    // shared color table; the discovery effect list constrains what HA offers
    uint8_t color = 0;
    for (int i = 1; i < LIGHTING_COLOR_COUNT; i++) {
        if ((int)strlen(LIGHTING_COLOR_NAMES[i]) == payload_len &&
            strncmp(payload, LIGHTING_COLOR_NAMES[i], payload_len) == 0) {
            color = (uint8_t)i;
            break;
        }
    }
    if (color == 0) {
        ESP_LOGE(TAG, "Unknown light color: %.*s", payload_len, payload);
        return;
    }

    uint8_t reg_id = REG_ID_LIGHT_ZONE_COLOR_0 + (zone - 1);

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 3A 0F B9 [REG_ID] 01 [COLOR] [CHECKSUM] 03
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x3A, 0x0F, 0xB9, // Command pattern
        reg_id,     // Register ID (light zone color)
        0x01,       // Slot ID
        color,      // Color code (shared color table)
        0x00,       // Checksum (calculated below)
        0x03        // END
    };

    // Calculate checksum (sum of bytes 10-12)
    cmd[13] = (reg_id + 0x01 + color) & 0xFF;

    ESP_LOGI(TAG, "Sending light zone %d color %s (0x%02X) command", zone,
             LIGHTING_COLOR_NAMES[color], color);
    send_uart_command(cmd, sizeof(cmd));
}

static void handle_light_resync_command(int zone)
{
    uint8_t zone_idx = zone - 1;

    // Build CMD 0x3C Light Resync broadcast, impersonating the Touchscreen
    // (the only observed sender of this command)
    // Pattern: 02 00 50 FF FF 80 00 3C 0D 19 [ZONE_IDX] [CHECKSUM] 03
    // Checksum = zone_idx (only data byte)
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0x50, // SOURCE: Touchscreen
        0xFF, 0xFF, // DEST: Broadcast
        0x80, 0x00, // CONTROL
        0x3C, 0x0D, 0x19, // Command pattern
        zone_idx,   // Zone index (0-based)
        zone_idx,   // Checksum (= zone index)
        0x03        // END
    };

    ESP_LOGI(TAG, "Sending light zone %d resync command", zone);
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Heater Control
// ======================================================

static void handle_heater_command(const char *payload, int payload_len, int index)
{
    ESP_LOGI(TAG, "Heater %d command: %.*s", index, payload_len, payload);

    // State register per heater: Heater 1 = 0xE6, Heater 2 = 0xE9.
    uint8_t reg;
    if (index == 0) {
        reg = REG_ID_HEATER1_ONOFF;
    } else if (index == 1) {
        reg = REG_ID_HEATER2_ONOFF;
    } else {
        ESP_LOGW(TAG, "No state register known for heater %d", index);
        return;
    }

    uint8_t state;
    if (strncmp(payload, "ON", payload_len) == 0) {
        state = 0x01;
    } else if (strncmp(payload, "OFF", payload_len) == 0) {
        state = 0x00;
    } else {
        ESP_LOGE(TAG, "Invalid heater command: %.*s (expected ON/OFF)", payload_len, payload);
        return;
    }

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 3A 0F B9 [REG] 00 [STATE] [CHECKSUM] 03
    // Checksum = reg + 0x00 + state
    uint8_t cmd[] = {
        0x02,             // START
        0x00, 0xF0,       // SOURCE: Internet Gateway
        0xFF, 0xFF,       // DEST: Broadcast
        0x80, 0x00,       // CONTROL
        0x3A, 0x0F, 0xB9, // Command pattern
        reg,              // Register ID (heater state)
        0x00,             // Slot
        state,            // State (0x00=Off, 0x01=On)
        (reg + 0x00 + state) & 0xFF, // Checksum
        0x03              // END
    };

    ESP_LOGI(TAG, "Sending heater %d %s command", index, state ? "ON" : "OFF");
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Favourite Control
// ======================================================

static void handle_favourite_command(const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Favourite command: %.*s", payload_len, payload);

    uint8_t value;
    if (strncmp(payload, "Pool", payload_len) == 0 && payload_len == 4) {
        value = FAVOURITE_POOL;
    } else if (strncmp(payload, "Spa", payload_len) == 0 && payload_len == 3) {
        value = FAVOURITE_SPA;
    } else if (strncmp(payload, "All Off", payload_len) == 0 && payload_len == 7) {
        value = FAVOURITE_ALL_OFF;
    } else if (strncmp(payload, "All Auto", payload_len) == 0 && payload_len == 8) {
        value = FAVOURITE_ALL_AUTO;
    } else if (strncmp(payload, "No Favourite", payload_len) == 0 && payload_len == 12) {
        // Status-only value (register 0x20 = 0xFF, no favourite active).
        // There is no bus command to deactivate a favourite: CMD 0x2A with
        // value 0xFF was tested and the controller ignores it.
        ESP_LOGI(TAG, "Ignoring 'No Favourite' selection (status-only)");
        return;
    } else {
        // Search enabled user favourites (indices 2–7 → values 0x02–0x07)
        value = 0xFF;
        if (s_pool_state_mutex &&
            xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            for (int i = 2; i < MAX_FAVOURITES; i++) {
                const favourite_t *fav = &s_pool_state.favourites[i];
                if (fav->enabled_valid && fav->enabled &&
                    fav->name_valid &&
                    (int)strlen(fav->name) == payload_len &&
                    strncmp(payload, fav->name, payload_len) == 0) {
                    value = (uint8_t)i;
                    break;
                }
            }
            xSemaphoreGive(s_pool_state_mutex);
        }
        if (value == 0xFF) {
            ESP_LOGE(TAG, "Unknown favourite: %.*s", payload_len, payload);
            return;
        }
    }

    // Build CMD 0x2A to Touchscreen (0x0050)
    // Pattern: 02 00 F0 00 50 80 00 2A 0D F9 [VALUE] [CHECKSUM] 03
    // Checksum = value (only data byte)
    uint8_t cmd[] = {
        0x02,       // START
        0x00, 0xF0, // SOURCE: Internet Gateway
        0x00, 0x50, // DEST: Touchscreen
        0x80, 0x00, // CONTROL
        0x2A, 0x0D, 0xF9, // Command pattern
        value,      // Favourite value
        value,      // Checksum (= value)
        0x03        // END
    };

    ESP_LOGI(TAG, "Sending favourite command 0x%02X", value);
    send_uart_command(cmd, sizeof(cmd));
}

// ======================================================
// Mode Control
// ======================================================

static bool payload_equals(const char *payload, int payload_len, const char *expected)
{
    size_t expected_len = strlen(expected);
    return payload_len >= 0 && (size_t)payload_len == expected_len &&
           memcmp(payload, expected, expected_len) == 0;
}

static bool send_mode_value(uint8_t mode_value)
{
    // CMD 0x15 Mode Set broadcast, impersonating the Touchscreen.
    uint8_t cmd[] = {
        0x02,
        0x00, 0x50,
        0xFF, 0xFF,
        0x80, 0x00,
        0x15, 0x0D, 0xF2,
        mode_value,
        mode_value,
        0x03
    };

    ESP_LOGI(TAG, "Sending mode switch to %s", mode_value == MODE_POOL ? "Pool" : "Spa");
    return send_uart_command(cmd, sizeof(cmd));
}

static bool send_valve_state_raw(uint8_t valve_num, uint8_t state)
{
    if (valve_num < 1 || valve_num > MAX_VALVE_SLOTS || state > 0x02) return false;

    uint8_t valve_idx = valve_num - 1;
    uint8_t cmd[] = {
        0x02,
        0x00, 0xF0,
        0xFF, 0xFF,
        0x80, 0x00,
        0x28, 0x0E, 0xA6,
        valve_idx,
        state,
        (valve_idx + state) & 0xFF,
        0x03
    };

    ESP_LOGI(TAG, "Setting valve %u to %s", valve_num,
             state == 0x02 ? "On" : (state == 0x01 ? "Auto" : "Off"));
    return send_uart_command(cmd, sizeof(cmd));
}

static bool get_spa_valve_state(bool *return_active, bool *jets_active,
                                bool *spa_mode)
{
    if (!s_pool_state_mutex) return false;
    if (xSemaphoreTake(s_pool_state_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) return false;

    const valve_state_t *spa_return = &s_pool_state.valves[SPA_RETURN_VALVE_NUM - 1];
    const valve_state_t *spa_jets = &s_pool_state.valves[SPA_JETS_VALVE_NUM - 1];
    bool configured = spa_return->configured && spa_jets->configured;
    if (configured) {
        if (return_active) *return_active = spa_return->active;
        if (jets_active) *jets_active = spa_jets->active;
        if (spa_mode) *spa_mode = s_pool_state.mode_valid && s_pool_state.mode == MODE_SPA;
    }
    xSemaphoreGive(s_pool_state_mutex);
    return configured;
}

static bool spa_transition_is_current(uint32_t sequence)
{
    return __atomic_load_n(&s_spa_transition_sequence, __ATOMIC_RELAXED) == sequence;
}

static void finish_spa_transition(uint32_t sequence)
{
    uint32_t expected = sequence;
    __atomic_compare_exchange_n(&s_spa_transition_running, &expected, 0, false,
                                __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

// Open one branch and obtain controller confirmation before any other branch
// is allowed to close. The overlap also covers actuator travel on controllers
// that report the requested state before the valve has physically finished.
static bool ensure_spa_valve_open(uint8_t valve_num, uint32_t sequence)
{
    bool return_active = false;
    bool jets_active = false;
    if (!get_spa_valve_state(&return_active, &jets_active, NULL)) return false;

    bool active = valve_num == SPA_RETURN_VALVE_NUM ? return_active : jets_active;
    if (!active) {
        if (!send_valve_state_raw(valve_num, 0x02)) return false;

        int confirm_polls = SPA_VALVE_CONFIRM_TIMEOUT_MS / 100;
        for (int poll = 0; poll < confirm_polls; poll++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (!spa_transition_is_current(sequence)) return false;
            if (get_spa_valve_state(&return_active, &jets_active, NULL) &&
                (valve_num == SPA_RETURN_VALVE_NUM ? return_active : jets_active)) {
                active = true;
                break;
            }
        }
        if (!active) {
            ESP_LOGE(TAG, "Valve %u did not confirm active; refusing to close another return path",
                     valve_num);
            return false;
        }
    }

    // Always keep an overlap, even if the controller already reported the
    // branch active. Its active bit is command/state feedback rather than a
    // physical end-stop signal, so the actuator may still be travelling.
    int overlap_polls = SPA_VALVE_SAFE_OVERLAP_MS / 100;
    for (int overlap = 0; overlap < overlap_polls; overlap++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!spa_transition_is_current(sequence)) return false;
    }

    if (!get_spa_valve_state(&return_active, &jets_active, NULL)) return false;
    return valve_num == SPA_RETURN_VALVE_NUM ? return_active : jets_active;
}

static void spa_transition_task(void *arg)
{
    spa_transition_task_args_t request = *(spa_transition_task_args_t *)arg;
    free(arg);
    bool safe_to_close = false;

    switch (request.transition) {
    case SPA_JETS_LOW:
        // This transition only opens paths, so no sequencing delay is needed.
        send_valve_state_raw(SPA_RETURN_VALVE_NUM, 0x02);
        if (spa_transition_is_current(request.sequence)) {
            send_valve_state_raw(SPA_JETS_VALVE_NUM, 0x02);
        }
        break;

    case SPA_JETS_OFF:
        safe_to_close = ensure_spa_valve_open(SPA_RETURN_VALVE_NUM, request.sequence);
        if (safe_to_close && spa_transition_is_current(request.sequence)) {
            send_valve_state_raw(SPA_JETS_VALVE_NUM, 0x00);
        }
        break;

    case SPA_JETS_HIGH:
        safe_to_close = ensure_spa_valve_open(SPA_JETS_VALVE_NUM, request.sequence);
        if (safe_to_close && spa_transition_is_current(request.sequence)) {
            send_valve_state_raw(SPA_RETURN_VALVE_NUM, 0x00);
        }
        break;

    case SPA_ENTER_MODE:
        safe_to_close = ensure_spa_valve_open(SPA_RETURN_VALVE_NUM, request.sequence);
        if (safe_to_close && spa_transition_is_current(request.sequence)) {
            send_mode_value(MODE_SPA);
        }
        break;

    case SPA_RAW_VALVE_CLOSE: {
        uint8_t other = request.valve_num == SPA_RETURN_VALVE_NUM
                            ? SPA_JETS_VALVE_NUM : SPA_RETURN_VALVE_NUM;
        safe_to_close = ensure_spa_valve_open(other, request.sequence);
        if (safe_to_close && spa_transition_is_current(request.sequence)) {
            send_valve_state_raw(request.valve_num, request.valve_state);
        }
        break;
    }
    }

    finish_spa_transition(request.sequence);
    vTaskDelete(NULL);
}

static void start_spa_transition(spa_transition_t transition,
                                 uint8_t valve_num, uint8_t valve_state)
{
    spa_transition_task_args_t *args = malloc(sizeof(*args));
    if (!args) {
        ESP_LOGE(TAG, "No memory for spa valve transition worker");
        return;
    }

    uint32_t sequence = __atomic_add_fetch(&s_spa_transition_sequence, 1, __ATOMIC_RELAXED);
    if (sequence == 0) {
        sequence = __atomic_add_fetch(&s_spa_transition_sequence, 1, __ATOMIC_RELAXED);
    }
    args->transition = transition;
    args->valve_num = valve_num;
    args->valve_state = valve_state;
    args->sequence = sequence;
    __atomic_store_n(&s_spa_transition_running, sequence, __ATOMIC_RELAXED);

    if (xTaskCreate(spa_transition_task, "spa_valves", 3072, args, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start spa valve transition worker");
        finish_spa_transition(sequence);
        free(args);
    }
}

static void handle_mode_command(const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Mode command: %.*s", payload_len, payload);

    // CMD 0x15 uses the same encoding as the 0x14 status: MODE_SPA/MODE_POOL
    uint8_t mode_value;
    if (payload_equals(payload, payload_len, "Pool")) {
        mode_value = MODE_POOL;
    } else if (payload_equals(payload, payload_len, "Spa")) {
        mode_value = MODE_SPA;
    } else {
        ESP_LOGE(TAG, "Invalid mode command: %.*s (expected Pool/Spa)", payload_len, payload);
        return;
    }

    if (mode_value == MODE_SPA) {
        bool return_active = false;
        bool jets_active = false;
        if (get_spa_valve_state(&return_active, &jets_active, NULL) &&
            !return_active && !jets_active) {
            ESP_LOGW(TAG, "Both spa returns are closed; opening Auxiliary 1 before Spa mode");
            start_spa_transition(SPA_ENTER_MODE, 0, 0);
            return;
        }
    }

    send_mode_value(mode_value);
}

// ======================================================
// Temperature Setpoint Control
// ======================================================

static void handle_temperature_command(bool is_pool, const char *payload, int payload_len)
{
    // Parse temperature value (Celsius)
    char temp_str[16];
    if (payload_len >= sizeof(temp_str)) {
        ESP_LOGE(TAG, "Temperature payload too long");
        return;
    }
    memcpy(temp_str, payload, payload_len);
    temp_str[payload_len] = '\0';

    char *endptr;
    long temp_parsed = strtol(temp_str, &endptr, 10);
    if (endptr == temp_str || *endptr != '\0') {
        ESP_LOGE(TAG, "Invalid temperature value: \"%s\"", temp_str);
        return;
    }
    ESP_LOGI(TAG, "%s setpoint command: %ld°C", is_pool ? "Pool" : "Spa", temp_parsed);

    // Validate temperature range (Celsius)
    if (temp_parsed < TEMP_SETPOINT_MIN_C || temp_parsed > TEMP_SETPOINT_MAX_C) {
        ESP_LOGE(TAG, "Temperature out of range: %ld°C (valid: %d-%d)",
                 temp_parsed, TEMP_SETPOINT_MIN_C, TEMP_SETPOINT_MAX_C);
        return;
    }
    int32_t temp_c = (int32_t)temp_parsed;  // safe: range-checked above

    // Build UART command
    // Pattern: 02 00 F0 FF FF 80 00 19 0F 98 [TARGET] [TEMP_C] [TEMP_C] [CHECKSUM] 03
    // TARGET: 0x01=Pool, 0x02=Spa
    // Temperature byte is repeated as part of the message format
    // Checksum = TARGET + TEMP_C + TEMP_C
    uint8_t target = is_pool ? 0x01 : 0x02;
    uint8_t temp_byte = (uint8_t)temp_c;
    uint8_t checksum = (target + temp_byte + temp_byte) & 0xFF;

    uint8_t cmd[] = {
        0x02,             // START
        0x00, 0xF0,       // SOURCE: Internet Gateway
        0xFF, 0xFF,       // DEST: Broadcast
        0x80, 0x00,       // CONTROL
        0x19, 0x0F, 0x98, // Command pattern
        target,           // Target (0x01=Pool, 0x02=Spa)
        temp_byte,        // Temperature °C
        temp_byte,        // Temperature °C (repeated)
        checksum,         // Checksum
        0x03              // END
    };

    ESP_LOGI(TAG, "Setting %s setpoint to %" PRId32 "°C", is_pool ? "pool" : "spa", temp_c);
    send_uart_command(cmd, sizeof(cmd));

    // Read the setpoint back: on many installs nothing else republishes it, so
    // this is what carries the new value through to Home Assistant.
    register_requester_read_back(
        is_pool ? REG_ID_HEATER1_POOL_SETPOINT : REG_ID_HEATER1_SPA_SETPOINT, 0x00);
}

// Per-heater setpoint write.
//  - Heater 1 (index 0): the system pool/spa setpoint command (CMD 0x19).
//  - Heater 2 (index 1): gateway register write (CMD 0x3A) to 0xEA (pool) / 0xEB (spa).
static void handle_heater_setpoint_command(int index, bool is_pool,
                                           const char *payload, int payload_len)
{
    if (index == 0) {
        handle_temperature_command(is_pool, payload, payload_len);
        return;
    }
    // index == 1 (Heater 2): gateway register write to 0xEA (pool) / 0xEB (spa).

    // Parse temperature value (Celsius)
    char temp_str[16];
    if (payload_len >= (int)sizeof(temp_str)) {
        ESP_LOGE(TAG, "Temperature payload too long");
        return;
    }
    memcpy(temp_str, payload, payload_len);
    temp_str[payload_len] = '\0';

    char *endptr;
    long temp_parsed = strtol(temp_str, &endptr, 10);
    if (endptr == temp_str || *endptr != '\0') {
        ESP_LOGE(TAG, "Invalid temperature value: \"%s\"", temp_str);
        return;
    }
    if (temp_parsed < TEMP_SETPOINT_MIN_C || temp_parsed > TEMP_SETPOINT_MAX_C) {
        ESP_LOGE(TAG, "Temperature out of range: %ld°C (valid: %d-%d)",
                 temp_parsed, TEMP_SETPOINT_MIN_C, TEMP_SETPOINT_MAX_C);
        return;
    }

    uint8_t reg  = is_pool ? REG_ID_HEATER2_POOL_SETPOINT : REG_ID_HEATER2_SPA_SETPOINT;
    uint8_t slot = 0x00;
    uint8_t val  = (uint8_t)temp_parsed;

    // Pattern: 02 00 F0 FF FF 80 00 3A 0F B9 [REG] [SLOT] [VAL] [CHECKSUM] 03
    uint8_t cmd[] = {
        0x02,             // START
        0x00, 0xF0,       // SOURCE: Internet Gateway
        0xFF, 0xFF,       // DEST: Broadcast
        0x80, 0x00,       // CONTROL
        0x3A, 0x0F, 0xB9, // Register write command pattern
        reg,              // Register ID
        slot,             // Slot
        val,              // Value °C
        (reg + slot + val) & 0xFF, // Checksum
        0x03              // END
    };

    ESP_LOGI(TAG, "Setting Heater 2 %s setpoint to %ld°C", is_pool ? "pool" : "spa", temp_parsed);
    send_uart_command(cmd, sizeof(cmd));

    register_requester_read_back(reg, slot);
}

// ======================================================
// Valve Control
// ======================================================

static void handle_valve_command(int valve_num, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Valve %d command: %.*s", valve_num, payload_len, payload);

    uint8_t state;
    if (payload_equals(payload, payload_len, "On")) {
        state = 0x02;
    } else if (payload_equals(payload, payload_len, "Auto")) {
        state = 0x01;
    } else if (payload_equals(payload, payload_len, "Off")) {
        state = 0x00;
    } else {
        ESP_LOGE(TAG, "Invalid valve command: %.*s (expected Off/Auto/On)", payload_len, payload);
        return;
    }

    // In Spa mode the primary pool-return actuator is closed. A raw command
    // that may close one auxiliary is therefore sequenced behind opening the
    // other one. Pool mode is safe because its primary return remains open.
    if (state != 0x02 &&
        (valve_num == SPA_RETURN_VALVE_NUM || valve_num == SPA_JETS_VALVE_NUM)) {
        bool return_active = false;
        bool jets_active = false;
        bool spa_mode = false;
        if (get_spa_valve_state(&return_active, &jets_active, &spa_mode) && spa_mode) {
            bool other_active = valve_num == SPA_RETURN_VALVE_NUM ? jets_active : return_active;
            if (!other_active) {
                ESP_LOGW(TAG, "Sequencing valve %d close behind opening the other spa return",
                         valve_num);
                start_spa_transition(SPA_RAW_VALVE_CLOSE, (uint8_t)valve_num, state);
                return;
            }
        }
    }

    send_valve_state_raw((uint8_t)valve_num, state);
}

static void handle_spa_jets_command(const char *payload, int payload_len)
{
    spa_transition_t transition;
    if (payload_equals(payload, payload_len, "Off")) {
        transition = SPA_JETS_OFF;
    } else if (payload_equals(payload, payload_len, "Low")) {
        transition = SPA_JETS_LOW;
    } else if (payload_equals(payload, payload_len, "High")) {
        transition = SPA_JETS_HIGH;
    } else {
        ESP_LOGE(TAG, "Invalid spa jets command: %.*s (expected Off/Low/High)",
                 payload_len, payload);
        return;
    }

    ESP_LOGI(TAG, "Spa jets command: %.*s", payload_len, payload);
    start_spa_transition(transition, 0, 0);
}

void mqtt_commands_enforce_spa_valve_safety(const pool_state_t *state)
{
    if (!state || !state->mode_valid || state->mode != MODE_SPA) return;

    const valve_state_t *spa_return = &state->valves[SPA_RETURN_VALVE_NUM - 1];
    const valve_state_t *spa_jets = &state->valves[SPA_JETS_VALVE_NUM - 1];
    if (!spa_return->configured || !spa_jets->configured ||
        spa_return->active || spa_jets->active) {
        return;
    }

    if (__atomic_load_n(&s_spa_transition_running, __ATOMIC_RELAXED) != 0) return;

    ESP_LOGE(TAG, "Unsafe Spa state observed: both auxiliary returns closed; opening Auxiliary 1");
    start_spa_transition(SPA_JETS_OFF, 0, 0);
}

// ======================================================
// Main Command Handler
// ======================================================

void mqtt_handle_command(const char *topic, int topic_len, const char *data, int data_len)
{
    // Get device ID for topic matching
    char device_id[32];
    mqtt_get_device_id(device_id, sizeof(device_id));

    // Build expected topic prefix
    char topic_prefix[64];
    snprintf(topic_prefix, sizeof(topic_prefix), "pool/%s/", device_id);
    int prefix_len = strlen(topic_prefix);

    // Check if topic matches our device
    if (topic_len < prefix_len || strncmp(topic, topic_prefix, prefix_len) != 0) {
        ESP_LOGW(TAG, "Topic does not match device ID");
        return;
    }

    // Extract command part (after device prefix)
    const char *cmd_topic = topic + prefix_len;
    int cmd_topic_len = topic_len - prefix_len;

    // Parse command type
    if (strncmp(cmd_topic, "channel/", 8) == 0 && cmd_topic_len > 12) {
        // Extract channel number (formats: "channel/N/set", "channel/N/power/set",
        // "channel/N/mode/set")
        char *endptr;
        int channel = (int)strtol(cmd_topic + 8, &endptr, 10);
        if (endptr == cmd_topic + 8 || *endptr != '/') {
            ESP_LOGE(TAG, "Invalid channel topic format: %s", cmd_topic);
            return;
        }
        if (channel < 1 || channel > MAX_CHANNELS) {
            ESP_LOGE(TAG, "Invalid channel number: %d", channel);
            return;
        }
        int suffix_len = cmd_topic_len - (int)(endptr - cmd_topic);
        if (suffix_len == 4 && strncmp(endptr, "/set", 4) == 0) {
            handle_channel_command(channel, data, data_len);
        } else if (suffix_len == 10 && strncmp(endptr, "/power/set", 10) == 0) {
            handle_channel_power_command(channel, data, data_len);
        } else if (suffix_len == 9 && strncmp(endptr, "/mode/set", 9) == 0) {
            handle_channel_mode_command(channel, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid channel topic format: %s", cmd_topic);
        }
    }
    else if (strncmp(cmd_topic, "light/", 6) == 0 && cmd_topic_len > 10) {
        // Extract zone number (formats: "light/N/set", "light/N/color/set",
        // "light/N/resync")
        char *endptr;
        int zone = (int)strtol(cmd_topic + 6, &endptr, 10);
        if (endptr == cmd_topic + 6 || *endptr != '/') {
            ESP_LOGE(TAG, "Invalid light topic format: %s", cmd_topic);
            return;
        }
        if (zone < 1 || zone > MAX_LIGHT_ZONES) {
            ESP_LOGE(TAG, "Invalid light zone: %d", zone);
            return;
        }
        int suffix_len = cmd_topic_len - (int)(endptr - cmd_topic);
        if (suffix_len == 4 && strncmp(endptr, "/set", 4) == 0) {
            handle_light_command(zone, data, data_len);
        } else if (suffix_len == 10 && strncmp(endptr, "/color/set", 10) == 0) {
            handle_light_color_command(zone, data, data_len);
        } else if (suffix_len == 7 && strncmp(endptr, "/resync", 7) == 0) {
            handle_light_resync_command(zone);
        } else {
            ESP_LOGE(TAG, "Invalid light topic format: %s", cmd_topic);
        }
    }
    else if (strncmp(cmd_topic, "valve/", 6) == 0 && cmd_topic_len > 8) {
        // Extract valve number (format: "valve/N/set")
        char *endptr;
        int valve = (int)strtol(cmd_topic + 6, &endptr, 10);
        if (endptr == cmd_topic + 6 || *endptr != '/') {
            ESP_LOGE(TAG, "Invalid valve topic format: %s", cmd_topic);
            return;
        }
        if (valve >= 1 && valve <= MAX_VALVE_SLOTS) {
            handle_valve_command(valve, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid valve number: %d", valve);
        }
    }
    else if (strncmp(cmd_topic, "heater/", 7) == 0) {
        // Suffixes: "/set" (on/off), "/pool_setpoint/set", "/spa_setpoint/set"
        char *endptr;
        int idx = (int)strtol(cmd_topic + 7, &endptr, 10);
        int suffix_len = cmd_topic_len - (int)(endptr - cmd_topic);
        if (endptr == cmd_topic + 7 || idx < 0 || idx >= MAX_HEATERS) {
            ESP_LOGE(TAG, "Invalid heater topic: %s", cmd_topic);
        } else if (suffix_len == 4 && strncmp(endptr, "/set", 4) == 0) {
            handle_heater_command(data, data_len, idx);
        } else if (suffix_len == 18 && strncmp(endptr, "/pool_setpoint/set", 18) == 0) {
            handle_heater_setpoint_command(idx, true, data, data_len);
        } else if (suffix_len == 17 && strncmp(endptr, "/spa_setpoint/set", 17) == 0) {
            handle_heater_setpoint_command(idx, false, data, data_len);
        } else {
            ESP_LOGE(TAG, "Invalid heater topic: %s", cmd_topic);
        }
    }
    else if (strncmp(cmd_topic, "system/power/set", 16) == 0) {
        handle_system_power_command(data, data_len);
    }
    else if (cmd_topic_len == 12 && strncmp(cmd_topic, "spa_jets/set", 12) == 0) {
        handle_spa_jets_command(data, data_len);
    }
    else if (strncmp(cmd_topic, "mode/set", 8) == 0) {
        handle_mode_command(data, data_len);
    }
    else if (strncmp(cmd_topic, "favourite/set", 13) == 0) {
        handle_favourite_command(data, data_len);
    }
    else {
        ESP_LOGW(TAG, "Unknown command topic: %.*s", cmd_topic_len, cmd_topic);
    }
}
