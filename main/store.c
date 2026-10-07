#include "plant.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nvs.h"
#include "esp_spiffs.h"

#define HISTORY_PATH "/history/events.csv"
#define HISTORY_MAX 4000000

int64_t store_load_last_auto(void)
{
    int64_t timestamp = 0;
    nvs_handle_t handle;
    if (nvs_open("plant", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_i64(handle, "last_auto", &timestamp);
        nvs_close(handle);
    }
    return timestamp;
}

void store_save_last_auto(int64_t timestamp)
{
    nvs_handle_t handle;
    if (nvs_open("plant", NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_i64(handle, "last_auto", timestamp) == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
}

void store_init(void)
{
    esp_vfs_spiffs_conf_t fs = {.base_path = "/history", .partition_label = "history", .max_files = 2, .format_if_mount_failed = true};
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&fs));
}

void store_load(plant_settings_t *s)
{
    *s = (plant_settings_t){.auto_enabled = false, .soil_low = 30, .soil_target = 45,
        .pump_seconds = 3, .soak_seconds = 60, .min_interval = 30, .max_daily = 5,
        .dry_adc = 0, .wet_adc = 0, .brightness = 100, .water_dry_adc = -1, .water_wet_adc = -1};
    strcpy(s->name, "SmartPlant");
    nvs_handle_t nvs;
    if (nvs_open("plant", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t length = sizeof(*s);
    plant_settings_t saved;
    if (nvs_get_blob(nvs, "settings", &saved, &length) == ESP_OK && length == sizeof(saved)) *s = saved;
    nvs_close(nvs);
}

esp_err_t store_save(const plant_settings_t *s)
{
    nvs_handle_t nvs;
    esp_err_t e = nvs_open("plant", NVS_READWRITE, &nvs);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(nvs, "settings", s, sizeof(*s));
    if (e == ESP_OK) e = nvs_commit(nvs);
    nvs_close(nvs);
    return e;
}

void store_load_daily(int64_t *day, int *count)
{
    *day = -1; *count = 0;
    nvs_handle_t nvs;
    if (nvs_open("plant", NVS_READONLY, &nvs) != ESP_OK) return;
    nvs_get_i64(nvs, "day", day);
    int32_t saved = 0;
    if (nvs_get_i32(nvs, "daily", &saved) == ESP_OK) *count = saved;
    nvs_close(nvs);
}

void store_save_daily(int64_t day, int count)
{
    nvs_handle_t nvs;
    if (nvs_open("plant", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_i64(nvs, "day", day);
    nvs_set_i32(nvs, "daily", count);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void append_row(const char *kind, const plant_status_t *s)
{
    time_t wall; time(&wall);
    if (wall <= 1700000000) return;
    FILE *f = fopen(HISTORY_PATH, "a");
    if (!f) return;
    fprintf(f, "%lld,%s,%d,%d,%d\n", (long long)wall, kind, s->soil_percent, s->soil_raw, s->tank_empty);
    long size = ftell(f);
    fclose(f);
    if (size <= HISTORY_MAX) return;
    FILE *in = fopen(HISTORY_PATH, "r");
    if (!in) return;
    fseek(in, size / 2, SEEK_SET);
    char line[96];
    fgets(line, sizeof(line), in);
    FILE *out = fopen("/history/trim.csv", "w");
    if (!out) { fclose(in); return; }
    while (fgets(line, sizeof(line), in)) fputs(line, out);
    fclose(in); fclose(out);
    rename("/history/trim.csv", HISTORY_PATH);
}

void store_sample(const plant_status_t *s) { if (s->soil_valid) append_row("sample", s); }
void store_event(const char *kind, const plant_status_t *s) { append_row(kind, s); }

esp_err_t store_history_send(int limit, int hours,
    esp_err_t (*send_chunk)(const char *, size_t, void *), void *context)
{
    if (limit < 1) limit = 1;
    if (limit > 500) limit = 500;
    if (hours != 24 && hours != 168 && hours != 720) hours = 24;
    time_t wall; time(&wall);
    long long cutoff = wall > 1700000000 ? (long long)wall - hours * 3600LL : 0;
    char json[1024];
    size_t used = 0;
    json[used++] = '[';
    FILE *f = fopen(HISTORY_PATH, "r");
    if (f) {
        char line[96], kind[24];
        long long ts;
        int percent, raw, empty;
        int samples = 0, events = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%lld,%23[^,],%d,%d,%d", &ts, kind, &percent, &raw, &empty) == 5 && ts >= cutoff) {
                if (!strcmp(kind, "sample")) samples++;
                else events++;
            }
        }
        rewind(f);
        int event_budget = events < limit / 3 ? events : limit / 3;
        if (!samples) event_budget = events < limit ? events : limit;
        int sample_budget = limit - event_budget;
        if (sample_budget > samples) sample_budget = samples;
        int seen = 0, event_seen = 0, sampled = 0, emitted = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%lld,%23[^,],%d,%d,%d", &ts, kind, &percent, &raw, &empty) != 5) continue;
            if (ts < cutoff) continue;
            if (!strcmp(kind, "sample")) {
                int index = seen++;
                if (!sample_budget || sampled >= sample_budget) continue;
                int target = sample_budget == 1 ? samples - 1 :
                    (int)((int64_t)sampled * (samples - 1) / (sample_budget - 1));
                if (index != target) continue;
                sampled++;
            } else if (event_seen++ < events - event_budget) continue;
            char row[112];
            int written = snprintf(row, sizeof(row),
                "%s{\"time\":%lld,\"type\":\"%s\",\"soil\":%d,\"raw\":%d,\"tank_empty\":%s}",
                emitted ? "," : "", ts, kind, percent, raw, empty ? "true" : "false");
            if (written < 0 || written >= sizeof(row)) { fclose(f); return ESP_FAIL; }
            if (used + written >= sizeof(json)) {
                esp_err_t result = send_chunk(json, used, context);
                if (result != ESP_OK) { fclose(f); return result; }
                used = 0;
            }
            memcpy(json + used, row, written);
            used += written;
            if (++emitted == limit) break;
        }
        fclose(f);
    }
    json[used++] = ']';
    return send_chunk(json, used, context);
}


int store_chart(int hours, int *values, size_t count)
{
    for (size_t i = 0; i < count; i++) values[i] = -1;
    if (!count || time(NULL) <= 1700000000) return 0;
    if (hours != 24 && hours != 168 && hours != 720) hours = 24;
    long long end = time(NULL), start = end - hours * 3600LL;
    int *sums = calloc(count, sizeof(int));
    int *counts = calloc(count, sizeof(int));
    if (!sums || !counts) { free(sums); free(counts); return 0; }
    for (size_t i = 0; i < count; i++) values[i] = -1;
    FILE *f = fopen(HISTORY_PATH, "r");
    int samples = 0;
    if (f) {
        char line[96], kind[24]; long long ts; int percent, raw, empty;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%lld,%23[^,],%d,%d,%d", &ts, kind, &percent, &raw, &empty) != 5 ||
                strcmp(kind, "sample") || ts < start || ts > end) continue;
            size_t bucket = (size_t)((ts - start) * (count - 1) / (end - start));
            sums[bucket] += percent; counts[bucket]++; samples++;
        }
        fclose(f);
    }
    for (size_t i = 0; i < count; i++) if (counts[i]) values[i] = sums[i] / counts[i];
    free(sums); free(counts);
    return samples;
}
