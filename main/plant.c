#include "plant.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t lock;
static plant_settings_t cfg;
static plant_status_t state;
static int64_t pump_deadline, pump_started, soak_deadline, last_auto_start, last_sample;
static int64_t day_key = -1;
static bool automatic_run;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void stop_locked(int64_t now)
{
    if (!state.pump_running) return;
    if (board_pump_set(false) != ESP_OK) { strcpy(state.alert, "PUMP_IO_FAULT"); return; }
    state.pump_running = false;
    state.remaining_ms = 0;
    state.today_seconds += (now - pump_started + 500) / 1000;
    if (automatic_run) {
        soak_deadline = now + cfg.soak_seconds * 1000LL;
        strcpy(state.mode, "soaking");
    } else {
        soak_deadline = now + cfg.soak_seconds * 1000LL;
        strcpy(state.mode, "soaking");
    }
    automatic_run = false;
    store_event("watering_end", &state);
}

void plant_init(const plant_settings_t *settings)
{
    lock = xSemaphoreCreateMutex();
    cfg = *settings;
    memset(&state, 0, sizeof(state));
    state.last_auto_time = store_load_last_auto();
    state.min_soil = 100;
    strcpy(state.mode, "idle");
    store_load_daily(&day_key, &state.today_count);
    if (state.today_count > 0) last_auto_start = now_ms();
    ESP_ERROR_CHECK(board_pump_set(false));
}

void plant_get_status(plant_status_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = state;
    out->uptime_ms = now_ms();
    int64_t now = out->uptime_ms;
    int64_t due = last_auto_start ? last_auto_start + cfg.min_interval * 60000LL : now;
    if (soak_deadline > due) due = soak_deadline;
    if (state.pump_running && pump_deadline + cfg.soak_seconds * 1000LL > due)
        due = pump_deadline + cfg.soak_seconds * 1000LL;
    out->next_auto_seconds = -1;
    out->next_auto_time = 0;
    const char *reason;
    if (!cfg.auto_enabled) reason = "disabled";
    else if (!state.soil_valid) reason = "sensor_fault";
    else if (state.today_count >= cfg.max_daily) reason = "daily_limit";
    else {
        out->next_auto_seconds = due > now ? (int)((due - now + 999) / 1000) : 0;
        time_t wall = time(NULL);
        if (wall > 1700000000) out->next_auto_time = wall + out->next_auto_seconds;
        reason = state.pump_running ? "watering" : due > now ? "cooldown" :
            state.soil_percent >= cfg.soil_low ? "wet" : "ready";
    }
    strcpy(out->auto_wait, reason);
    xSemaphoreGive(lock);
}

void plant_get_settings(plant_settings_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = cfg;
    xSemaphoreGive(lock);
}

esp_err_t plant_set_settings(const plant_settings_t *next)
{
    if (next->soil_low < 5 || next->soil_low > 80 || next->soil_target < next->soil_low + 5 || next->soil_target > 95 ||
        next->pump_seconds < 1 || next->pump_seconds > 10 || next->soak_seconds < 10 || next->soak_seconds > 600 ||
        next->min_interval < 5 || next->min_interval > 1440 || next->max_daily < 1 || next->max_daily > 20 ||
        next->dry_adc < 0 || next->dry_adc > 4095 || next->wet_adc < 0 || next->wet_adc > 4095 ||
        next->water_dry_adc < -1 || next->water_dry_adc > 4095 ||
        next->water_wet_adc < -1 || next->water_wet_adc > 4095 ||
        next->brightness < 0 || next->brightness > 100 || !memchr(next->name, 0, sizeof(next->name))) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t e = memcmp(&cfg, next, sizeof(cfg)) ? store_save(next) : ESP_OK;
    if (e == ESP_OK) {
        cfg = *next;
    }
    xSemaphoreGive(lock);
    return e;
}

esp_err_t plant_request_pump(int seconds, bool automatic)
{
    if (seconds < 1 || seconds > 10) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    int64_t now = now_ms();
    esp_err_t e = ESP_OK;
    if (state.pump_running) e = ESP_ERR_INVALID_STATE;
    else if (automatic && (!cfg.auto_enabled || !state.soil_valid || state.today_count >= cfg.max_daily ||
        (last_auto_start && now - last_auto_start < cfg.min_interval * 60000LL))) e = ESP_ERR_INVALID_STATE;
    if (e == ESP_OK) e = board_pump_set(true);
    if (e == ESP_OK) {
        automatic_run = automatic;
        pump_started = now;
        pump_deadline = now + seconds * 1000LL;
        state.pump_running = true;
        state.remaining_ms = seconds * 1000;
        strcpy(state.mode, automatic ? "auto" : "manual");
        if (automatic) {
            last_auto_start = now;
            time_t wall = time(NULL);
            state.last_auto_time = wall > 1700000000 ? wall : 0;
            if (state.last_auto_time) store_save_last_auto(state.last_auto_time);
            state.today_count++;
            store_save_daily(day_key, state.today_count);
        }
        store_event(automatic ? "auto_start" : "manual_start", &state);
    }
    xSemaphoreGive(lock);
    return e;
}

void plant_stop_pump(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int64_t now = now_ms();
    pump_deadline = now; // Retry a failed manual stop on the next tick.
    stop_locked(now);
    xSemaphoreGive(lock);
}

esp_err_t plant_calibrate(bool wet)
{
    int raw = board_read_soil();
    if (raw < 0) return ESP_FAIL;
    plant_settings_t next;
    plant_get_settings(&next);
    if (wet) next.wet_adc = raw;
    else next.dry_adc = raw;
    return plant_set_settings(&next);
}

void plant_tick(void)
{
    int raw = board_read_soil();
    int water = board_read_water();
    int64_t now = now_ms();
    time_t wall; time(&wall);
    struct tm local;
    localtime_r(&wall, &local);
    int64_t today = wall > 1700000000 ? (local.tm_year * 366LL + local.tm_yday) : -1;
    bool should_start = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool was_valid = state.soil_valid;
    if (today >= 0 && day_key != today) {
        bool first_sync_with_count = day_key < 0 && state.today_count > 0;
        day_key = today;
        if (!first_sync_with_count) {
            state.today_count = 0; state.today_seconds = 0; state.min_soil = 100; state.max_soil = 0;
        }
        store_save_daily(day_key, state.today_count);
    }
    state.water_raw = water;
    state.water_valid = water >= 0 && cfg.water_dry_adc >= 0 && cfg.water_wet_adc >= 0 &&
        abs(cfg.water_wet_adc - cfg.water_dry_adc) >= 100;
    if (state.water_valid) {
        int percent = (water - cfg.water_dry_adc) * 100 / (cfg.water_wet_adc - cfg.water_dry_adc);
        state.water_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    }
    state.soil_raw = raw;
    state.soil_valid = raw >= 0 && cfg.dry_adc != cfg.wet_adc &&
        raw >= (cfg.dry_adc < cfg.wet_adc ? cfg.dry_adc : cfg.wet_adc) - 300 &&
        raw <= (cfg.dry_adc > cfg.wet_adc ? cfg.dry_adc : cfg.wet_adc) + 300;
    if (state.soil_valid) {
        int percent = (raw - cfg.dry_adc) * 100 / (cfg.wet_adc - cfg.dry_adc);
        state.soil_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
        if (state.soil_percent < state.min_soil) state.min_soil = state.soil_percent;
        if (state.soil_percent > state.max_soil) state.max_soil = state.soil_percent;
    }
    if (!state.soil_valid) strcpy(state.alert, "SENSOR_FAULT");
    else if (state.today_count >= cfg.max_daily) strcpy(state.alert, "DAILY_LIMIT");
    else state.alert[0] = 0;
    if (!state.soil_valid && was_valid) store_event("sensor_fault", &state);
    if (state.pump_running) {
        if (now >= pump_deadline) stop_locked(now);
        else state.remaining_ms = pump_deadline - now;
    }
    if (!state.pump_running && soak_deadline && now >= soak_deadline) {
        soak_deadline = 0;
        strcpy(state.mode, "idle");
    }
    should_start = !state.pump_running && !soak_deadline && cfg.auto_enabled && state.soil_valid &&
        state.soil_percent < cfg.soil_low && state.today_count < cfg.max_daily &&
        (!last_auto_start || now - last_auto_start >= cfg.min_interval * 60000LL);
    if (wall > 1700000000 && (!last_sample || now - last_sample >= 60000)) {
        store_sample(&state); last_sample = now;
    }
    xSemaphoreGive(lock);
    if (should_start) plant_request_pump(cfg.pump_seconds, true);
}

esp_err_t plant_calibrate_water(bool wet)
{
    int raw = board_read_water();
    if (raw < 0) return ESP_FAIL;
    plant_settings_t next;
    plant_get_settings(&next);
    if (wet) next.water_wet_adc = raw;
    else next.water_dry_adc = raw;
    return plant_set_settings(&next);
}

const char *plant_error_name(esp_err_t error)
{
    if (error == ESP_ERR_INVALID_ARG) return "INVALID_ARGUMENT";
    if (error == ESP_ERR_INVALID_STATE) return "PUMP_REJECTED";
    return "DEVICE_ERROR";
}
