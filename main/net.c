#include "plant.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "mdns.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "network";
static esp_netif_t *station, *ap;
static bool connected, setup_mode;
static int failures, disconnect_reason;
static bool wifi_ready, connecting;
static SemaphoreHandle_t operation_lock;
static bool mdns_started;
static bool sntp_started;
static char ip_text[20] = "0.0.0.0";
static char ssid_text[33] = "";

static void setup_ap(void)
{
    if (setup_mode) return;
    if (!ap) ap = esp_netif_create_default_wifi_ap();
    wifi_config_t config = {.ap = {.ssid = "SmartPlant_Setup", .ssid_len = 16,
        .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    setup_mode = true;
    strcpy(ip_text, "192.168.4.1");
    ESP_LOGI(TAG, "Setup AP enabled: SmartPlant_Setup, http://192.168.4.1");
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        strcpy(ip_text, setup_mode ? "192.168.4.1" : "0.0.0.0");
        wifi_event_sta_disconnected_t *event = data;
        // Switching networks deliberately disconnects the old station.
        if (event->reason == WIFI_REASON_ASSOC_LEAVE) return;
        disconnect_reason = event->reason;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", event->reason);
        if (!ssid_text[0]) return;
        if (++failures >= 5) {
            // Stop channel scans after falling back, so the setup AP stays discoverable.
            connecting = false;
            setup_ap();
            return;
        }
        connecting = true;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
        connecting = false;
        disconnect_reason = 0;
        failures = 0;
        ip_event_got_ip_t *got = data;
        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&got->ip_info.ip));
        ESP_LOGI(TAG, "Connected. Open http://%s/ or http://smartplant.local", ip_text);
        if (setup_mode) { ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); setup_mode = false; }
        if (!mdns_started && mdns_init() == ESP_OK) {
            mdns_hostname_set("smartplant");
            mdns_instance_name_set("SmartPlant");
            mdns_started = true;
        }
        if (!sntp_started) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&config) == ESP_OK) sntp_started = true;
        }
    }
}

static void log_target_network(void)
{
    wifi_scan_config_t scan = {.ssid = (uint8_t *)ssid_text, .show_hidden = true};
    esp_err_t error = esp_wifi_scan_start(&scan, true);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Initial Wi-Fi scan failed: %s", esp_err_to_name(error));
        return;
    }
    wifi_ap_record_t record;
    uint16_t count = 1;
    error = esp_wifi_scan_get_ap_records(&count, &record);
    if (error == ESP_OK && count) {
        ESP_LOGI(TAG, "Target SSID found on 2.4 GHz: channel=%u, RSSI=%d", record.primary, record.rssi);
    } else {
        ESP_LOGW(TAG, "Target SSID not found on 2.4 GHz. ESP32-S3 cannot use 5 GHz-only networks.");
    }
}

void net_start(void)
{
    operation_lock = xSemaphoreCreateMutex();
    assert(operation_lock);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    station = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t saved = {0};
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &saved));
    if (!saved.sta.ssid[0] && CONFIG_SMARTPLANT_WIFI_SSID[0]) {
        size_t ssid_len = strlen(CONFIG_SMARTPLANT_WIFI_SSID);
        size_t password_len = strlen(CONFIG_SMARTPLANT_WIFI_PASSWORD);
        ESP_ERROR_CHECK(ssid_len <= sizeof(saved.sta.ssid) && password_len < sizeof(saved.sta.password)
            ? ESP_OK : ESP_ERR_INVALID_ARG);
        memcpy(saved.sta.ssid, CONFIG_SMARTPLANT_WIFI_SSID, ssid_len);
        memcpy(saved.sta.password, CONFIG_SMARTPLANT_WIFI_PASSWORD, password_len);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &saved));
        ESP_LOGI(TAG, "Initial home Wi-Fi credentials saved.");
    }
    memcpy(ssid_text, saved.sta.ssid, sizeof(saved.sta.ssid));
    ssid_text[32] = 0;
    if (!ssid_text[0]) setup_ap();
    ESP_ERROR_CHECK(esp_wifi_start());
    if (ssid_text[0]) {
        log_target_network();
        connecting = true;
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
    wifi_ready = true;
}

bool net_connected(void) { return connected; }
bool net_ap_mode(void) { return setup_mode; }
const char *net_ip(void) { return ip_text; }
const char *net_ssid(void) { return setup_mode && !connected ? "SmartPlant_Setup" : ssid_text; }
int net_rssi(void)
{
    wifi_ap_record_t record;
    return connected && esp_wifi_sta_get_ap_info(&record) == ESP_OK ? record.rssi : 0;
}

const char *net_status_text(void)
{
    if (!wifi_ready) return "网络正在初始化";
    if (connected) return "已连接家庭网络";
    if (connecting) return "正在连接，请稍候...";
    if (disconnect_reason == WIFI_REASON_AUTH_FAIL ||
        disconnect_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        disconnect_reason == WIFI_REASON_HANDSHAKE_TIMEOUT)
        return "认证失败，请检查密码";
    if (disconnect_reason == WIFI_REASON_NO_AP_FOUND)
        return "找不到网络，请重新扫描";
    if (disconnect_reason) return "连接失败，请重新选择网络";
    return "尚未连接，请选择网络";
}

esp_err_t net_scan(net_network_t *networks, int capacity, int *count)
{
    *count = 0;
    if (!wifi_ready) return ESP_ERR_INVALID_STATE;
    if (connecting) return ESP_ERR_WIFI_STATE;
    if (!xSemaphoreTake(operation_lock, 0)) return ESP_ERR_WIFI_STATE;
    esp_err_t error = esp_wifi_scan_start(NULL, true);
    if (error == ESP_OK) {
        uint16_t total = 16;
        wifi_ap_record_t records[16];
        error = esp_wifi_scan_get_ap_records(&total, records);
        if (error == ESP_OK) {
            for (int i = 0; i < total && *count < capacity; i++) {
                if (!records[i].ssid[0]) continue;
                bool duplicate = false;
                for (int j = 0; j < *count; j++)
                    if (!strcmp(networks[j].ssid, (char *)records[i].ssid)) duplicate = true;
                if (duplicate) continue;
                net_network_t *row = &networks[(*count)++];
                memcpy(row->ssid, records[i].ssid, 32);
                row->ssid[32] = 0;
                row->rssi = records[i].rssi;
                row->open = records[i].authmode == WIFI_AUTH_OPEN;
            }
        } else esp_wifi_clear_ap_list();
    }
    xSemaphoreGive(operation_lock);
    return error;
}

char *net_scan_json(void)
{
    net_network_t networks[16];
    int count;
    if (net_scan(networks, 16, &count) != ESP_OK) return NULL;
    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(row, "rssi", networks[i].rssi);
        cJSON_AddBoolToObject(row, "open", networks[i].open);
        cJSON_AddItemToArray(array, row);
    }
    char *result = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return result;
}

esp_err_t net_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !password || strlen(password) > 63) return ESP_ERR_INVALID_ARG;
    if (!wifi_ready) return ESP_ERR_INVALID_STATE;
    if (!xSemaphoreTake(operation_lock, 0)) return ESP_ERR_WIFI_STATE;
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    esp_err_t e = esp_wifi_disconnect();
    if (e == ESP_ERR_WIFI_NOT_CONNECT) e = ESP_OK;
    if (e == ESP_OK) {
        connected = connecting = false;
        strcpy(ip_text, setup_mode ? "192.168.4.1" : "0.0.0.0");
        e = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (e == ESP_OK) {
        strcpy(ssid_text, ssid);
        connected = false;
        failures = disconnect_reason = 0;
        strcpy(ip_text, setup_mode ? "192.168.4.1" : "0.0.0.0");
        connecting = true;
        e = esp_wifi_connect();
        if (e != ESP_OK) connecting = false;
    }
    memset(config.sta.password, 0, sizeof(config.sta.password));
    xSemaphoreGive(operation_lock);
    return e;
}
