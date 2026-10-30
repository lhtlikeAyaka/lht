# The main component owns the sketch translation unit and explicitly links
# the managed Arduino, ESP-SR and ArduinoJson components.
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        espressif__arduino-esp32
        espressif__esp-sr
        bblanchon__arduinojson
        esp_partition
        esp_system
        esp_common
        efuse
        heap
        log
        mbedtls
        freertos
)
