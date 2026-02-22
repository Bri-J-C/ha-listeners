idf_component_register(
    SRCS
        "main.c"
        "audio_input.c"
        "audio_output.c"
        "aec.c"
        "codec.c"
        "network.c"
        "discovery.c"
        "button.c"
        "settings.c"
        "webserver.c"
        "ha_mqtt.c"
        "diagnostics.c"
        "display.c"
        "agc.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_wifi
        esp_netif
        nvs_flash
        esp_event
        esp_timer
        lwip
        json
        esp_http_server
        app_update
        mqtt
        mdns
)

# Add libopus as a managed component
# Run: idf.py add-dependency "espressif/libopus"
