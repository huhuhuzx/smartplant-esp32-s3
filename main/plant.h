#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    bool auto_enabled;
    int soil_low, soil_target, pump_seconds, soak_seconds, min_interval, max_daily;
    int dry_adc, wet_adc;
    int brightness;
    char name[32];
    int water_dry_adc, water_wet_adc;
} plant_settings_t;

typedef struct {
    int soil_raw, soil_percent;
    int water_raw, water_percent;
    bool water_valid;
    bool soil_valid, tank_empty, pump_running;
    int remaining_ms, today_count, today_seconds;
    int min_soil, max_soil;
    int64_t uptime_ms;
    int64_t last_auto_time, next_auto_time;
    int next_auto_seconds;
    char auto_wait[20];
    char mode[16], alert[32];
} plant_status_t;

void plant_init(const plant_settings_t *settings);
void plant_tick(void);
void plant_get_status(plant_status_t *out);
void plant_get_settings(plant_settings_t *out);
esp_err_t plant_set_settings(const plant_settings_t *settings);
esp_err_t plant_request_pump(int seconds, bool automatic);
void plant_stop_pump(void);
esp_err_t plant_calibrate(bool wet);
esp_err_t plant_calibrate_water(bool wet);
const char *plant_error_name(esp_err_t error);

void board_init(void);
int board_read_soil(void);
esp_err_t board_pump_set(bool on);
int board_read_water(void);

void store_init(void);
void store_load(plant_settings_t *settings);
esp_err_t store_save(const plant_settings_t *settings);
void store_load_daily(int64_t *day, int *count);
void store_save_daily(int64_t day, int count);
int64_t store_load_last_auto(void);
void store_save_last_auto(int64_t timestamp);
void store_sample(const plant_status_t *status);
void store_event(const char *kind, const plant_status_t *status);
int store_chart(int hours, int *values, size_t count);
esp_err_t store_history_send(int limit, int hours,
    esp_err_t (*send_chunk)(const char *, size_t, void *), void *context);

void net_start(void);
bool net_connected(void);
bool net_ap_mode(void);
int net_rssi(void);
const char *net_ip(void);
const char *net_ssid(void);
typedef struct {
    char ssid[33];
    int rssi;
    bool open;
} net_network_t;
const char *net_status_text(void);
esp_err_t net_scan(net_network_t *networks, int capacity, int *count);
char *net_scan_json(void);
esp_err_t net_save_credentials(const char *ssid, const char *password);

void web_start(void);
void web_broadcast(void);
void screen_init(void);
void screen_tick(void);
