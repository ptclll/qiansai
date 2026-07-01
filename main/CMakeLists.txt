# Embed CA, certificate & key directly into binary
idf_component_register(SRCS "wifi_eap_fast_main.c"
                           "nmea_parser.c"
                           "ak09911c.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_wifi nvs_flash esp_driver_uart esp_driver_gpio esp_driver_i2c tcp_transport)
