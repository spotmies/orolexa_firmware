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
        driver
        esp_wifi_remote
        usb_host_uvc
        mdns
)