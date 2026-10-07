#include "plant.h"
#include "bsp/esp32_s3_lcd_ev_board.h"
#include "esp_io_expander.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

// V1.5 J5 pin 12 is TCA9554 P4 / EX_IO4, not an ESP GPIO.
#define PUMP_PIN IO_EXPANDER_PIN_NUM_4
static esp_io_expander_handle_t outputs;
static i2c_master_dev_handle_t ads;
static SemaphoreHandle_t adc_lock;

void board_init(void)
{
    ESP_ERROR_CHECK(bsp_i2c_init());
    outputs = bsp_io_expander_init();
    assert(outputs);
    // Set the output latch low before enabling the pump pin as an output.
    // set_level() rejects input pins. Use the documented register callbacks to
    // prime the latch, including the driver's cache, while P4 is still high-Z.
    uint32_t latch;
    ESP_ERROR_CHECK(outputs->read_output_reg(outputs, &latch));
    ESP_ERROR_CHECK(outputs->write_output_reg(outputs, latch & ~PUMP_PIN));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(outputs, PUMP_PIN, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(outputs, PUMP_PIN, 0));
    adc_lock = xSemaphoreCreateMutex();
    assert(adc_lock);
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x48,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &ads));
    ESP_LOGI("board", "V1.5: ADS1115 A0=soil, A1=water; J5 EX_IO4=pump");
}

static int read_channel(int channel)
{
    if (!ads) return -1;
    xSemaphoreTake(adc_lock, portMAX_DELAY);
    int sum = 0;
    for (int sample = 0; sample < 4; sample++) {
        // Single-ended, +/-4.096V, single-shot, 128 samples/s, comparator off.
        uint16_t config = 0x8383 | ((4 + channel) << 12);
        uint8_t command[] = {1, config >> 8, config & 255};
        if (i2c_master_transmit(ads, command, sizeof(command), 40) != ESP_OK) goto failed;
        bool ready = false;
        for (int retry = 0; retry < 4; retry++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            uint8_t reg = 1, data[2];
            if (i2c_master_transmit_receive(ads, &reg, 1, data, 2, 40) != ESP_OK) goto failed;
            if (data[0] & 0x80) { ready = true; break; }
        }
        if (!ready) goto failed;
        uint8_t reg = 0, data[2];
        if (i2c_master_transmit_receive(ads, &reg, 1, data, 2, 40) != ESP_OK) goto failed;
        int16_t value = (int16_t)((data[0] << 8) | data[1]);
        // A common 0..4095 scale represents 0..3.3V. Keep soil calibration semantic.
        int raw = value <= 0 ? 0 : value * 4095 / 26400;
        sum += raw > 4095 ? 4095 : raw;
    }
    xSemaphoreGive(adc_lock);
    return sum / 4;
failed:
    xSemaphoreGive(adc_lock);
    return -1;
}

int board_read_soil(void) { return read_channel(0); }
int board_read_water(void) { return read_channel(1); }
esp_err_t board_pump_set(bool on)
{
    return esp_io_expander_set_level(outputs, PUMP_PIN, on ? 1 : 0);
}
