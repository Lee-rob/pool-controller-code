idf_component_register(
    SRCS
        "main.c"
        "bus.c"
        "device_serial.c"
        "led_helper.c"
        "wifi_provisioning.c"
        "mqtt_poolclient.c"
        "mqtt_publish.c"
        "mqtt_discovery.c"
        "mqtt_commands.c"
        "web_handlers.c"
        "tcp_bridge.c"
        "framing.c"
        "lighting_colors.c"
        "message_decoder.c"
        "register_requester.c"
        "channel_power.c"
        "filter_pump_type.c"
        "channel_states.c"
        "channel_energy.c"
        "unknown_buffer.c"
        "dns_server.c"
        "heap_monitor.c"
        "firmware_update.c"
        "heater_freshness.c"
    INCLUDE_DIRS
         "."
    EMBED_TXTFILES
        "static/favicon.svg"
        "static/oat.min.css"
        "static/oat.min.js"
        "static/status_view.css"
        "static/status_view.js"
    EMBED_FILES
        "static/favicon.png"
    REQUIRES
        esp_app_format
        app_update
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_http_client
        esp_https_ota
        esp-tls
        mbedtls
        nvs_flash
        driver
        mqtt
        mdns
        led_strip
        json
)

# Preserve target-compiler stack measurements for release checks. The heater
# worker must also fail compilation if a full pool_state_t local is reintroduced.
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
set_source_files_properties("heater_freshness.c" PROPERTIES
    COMPILE_OPTIONS "-Werror=frame-larger-than=1024")
set_source_files_properties("mqtt_discovery.c" PROPERTIES
    COMPILE_OPTIONS "-Werror=frame-larger-than=2048")
# The bridge work buffers must not creep back onto its nested decode stack.
set_source_files_properties("tcp_bridge.c" PROPERTIES
    COMPILE_OPTIONS "-Werror=frame-larger-than=768")
