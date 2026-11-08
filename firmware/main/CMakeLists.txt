idf_component_register(SRCS "app_main.c" "tps_bus.c" "tps_selftest.c" "safety_sm.c"
                                                "lightbar_out.c" "rgb_status.c" "solar_policy.c" "lb_ota.c"
                                                "wifi_prov.c" "web_ui.c" "evlog.c" "app_matter.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES tps92390 lightbar_mix sht41 ws2812 lb_solar lb_time esp_driver_gpio esp_driver_i2c
                                spi_flash nvs_flash app_update esp_http_client esp_http_server esp_wifi esp_netif
                       EMBED_TXTFILES "web/index.html" "web/style.css" "web/app.js")
