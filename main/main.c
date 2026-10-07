#include "plant.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void control_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    int half_seconds = 0;
    for (;;) {
        plant_tick();
        if (++half_seconds == 2) {
            web_broadcast();
            half_seconds = 0;
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(500));
    }
}

static void ui_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        screen_tick();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    setenv("TZ", "CST-8", 1);
    tzset();
    board_init();
    store_init();
    plant_settings_t settings;
    store_load(&settings);
    plant_init(&settings);
    screen_init();
    xTaskCreate(control_task, "control", 6144, NULL, 5, NULL);
    xTaskCreate(ui_task, "ui", 8192, NULL, 3, NULL);
    net_start();
    web_start();
    ESP_LOGI("smartplant", "control and web started");
}
