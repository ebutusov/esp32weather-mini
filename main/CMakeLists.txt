idf_component_register(SRCS "main.cpp" "config.cpp" "wifi.cpp" "http.cpp" "error_screen.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES epd spiffs nvs_flash esp_wifi esp_netif esp_event
                                esp_http_client esp_hw_support)

# The device configuration lives in spiffs/config.json, which is git-ignored.
# Fail early with a clear message instead of flashing an image without it.
set(spiffs_dir ${CMAKE_CURRENT_SOURCE_DIR}/../spiffs)
if(NOT EXISTS ${spiffs_dir}/config.json)
    message(FATAL_ERROR
        "spiffs/config.json not found.\n"
        "Copy spiffs/config.example.json to spiffs/config.json and fill in your "
        "WiFi credentials and server URL, then build again.")
endif()

# Packs spiffs/ into the "storage" partition; `idf.py flash` writes it too.
spiffs_create_partition_image(storage ${spiffs_dir} FLASH_IN_PROJECT)
