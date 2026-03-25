idf_component_register(
    SRCS "wifi_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event nvs_flash esp_netif lwip
)
