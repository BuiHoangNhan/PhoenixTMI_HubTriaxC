idf_component_register(SRCS "hub_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event nvs_flash esp_driver_uart)
