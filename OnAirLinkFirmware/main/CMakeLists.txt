idf_component_register(
  SRCS "main.cpp" "oled.cpp" "lamp_control.cpp"
  INCLUDE_DIRS "."
  REQUIRES driver esp_driver_i2c esp_wifi nvs_flash esp_netif esp_event esp_timer esp_http_server
)
