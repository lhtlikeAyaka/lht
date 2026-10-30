/*
 * ESP-IDF application entry for the Arduino-style voice assistant sketch.
 *
 * Do not enable CONFIG_AUTOSTART_ARDUINO here: this file owns app_main(),
 * initializes the Arduino core, then runs setup()/loop() on an 8 KB Arduino
 * task. The existing sketch is compiled as part of this translation unit.
 */
#include <Arduino.h>
#include <esp32-hal.h>
#include <esp_log.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../VoiceAssistant/VoiceAssistant.ino"

static void voiceAssistantLoopTask(void *parameter) {
    (void)parameter;
    setup();
    for (;;) {
        loop();
        // The sketch already calls delay(2); this extra yield keeps the task
        // cooperative if that delay is changed during future development.
        taskYIELD();
    }
}

extern "C" void app_main(void) {
    initArduino();

    BaseType_t created = xTaskCreateUniversal(
        voiceAssistantLoopTask,
        "voice-assistant",
        8192,
        nullptr,
        1,
        nullptr,
        ARDUINO_RUNNING_CORE
    );

    if (created != pdPASS) {
        // Leave a clear diagnostic in the boot log instead of silently
        // starting without the application task.
        ESP_LOGE("BOOT", "Failed to create voice assistant task");
        abort();
    }
}
