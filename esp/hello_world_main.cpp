/* Hello World Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"


#include <stdio.h>

#include "fix32.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define SCREEN_HEIGHT CONFIG_HEIGHT
#define SCREEN_WIDTH CONFIG_WIDTH
#define HUD_HEIGHT 8
#include <driver/gpio.h>
#include <driver/spi_master.h>

extern "C" {
    //#include "st7789.h"
    #include "ili9340.h"
    void app_main(void);
#if CONFIG_BLUETOOTH_GAMEPAD
    bool bluetooth_gamepad_init(void);
#endif
}

TFT_t dev;
#include "backend.c"
#include "main.cpp"

void _main(void *pvParameters) {
    show_splore_stand_in();
    while (true)
        vTaskDelay(pdMS_TO_TICKS(1000));
    pico8();
}
void app_main(void)
{
    printf("Hello world!\n");

    esp_err_t uart_result = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    if (uart_result != ESP_OK && uart_result != ESP_ERR_INVALID_STATE)
        printf("UART keyboard input unavailable: %s\n", esp_err_to_name(uart_result));
#if CONFIG_BLUETOOTH_GAMEPAD
    bluetooth_gamepad_init();
#endif
    init_wifi();

    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), WiFi%s%s, ",
            CONFIG_IDF_TARGET,
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    printf("silicon revision %d, ", chip_info.revision);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    printf("%dMB %s flash\n", flash_size / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %d bytes\n", esp_get_minimum_free_heap_size());
    q = xQueueCreate(1, sizeof(uint8_t));
	xTaskCreate(_main, "main", 1024*6, NULL, 2, NULL);
	xTaskCreatePinnedToCore(put_buffer, "put_buffer", 1024*6, NULL, 2, NULL, 1);

}
