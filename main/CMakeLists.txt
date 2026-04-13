idf_component_register(
    SRCS
        "main.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_timer
        esp_wifi
        esp_http_server
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_wifi_remote
        usb_host_uvc
        mdns
)