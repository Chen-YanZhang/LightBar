idf_component_register(SRCS "ws2812.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_rmt esp_driver_gpio esp_common)
