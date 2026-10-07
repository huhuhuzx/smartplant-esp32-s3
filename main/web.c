#include "plant.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_http_server.h"
#include "cJSON.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char styles_css_start[] asm("_binary_styles_css_start");
extern const char styles_css_end[] asm("_binary_styles_css_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");

static httpd_handle_t server;
#define WEB_MAX_CLIENTS 7

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t e = httpd_resp_sendstr(req, text);
    free(text);
    return e;
}

static esp_err_t error(httpd_req_t *req, int status, const char *code, const char *message)
{
    httpd_resp_set_status(req, status == 400 ? "400 Bad Request" : status == 409 ? "409 Conflict" : "500 Internal Server Error");
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", false);
    cJSON_AddStringToObject(json, "code", code);
    cJSON_AddStringToObject(json, "message", message);
    return send_json(req, json);
}

static cJSON *read_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) return NULL;
    char buffer[1025];
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (n <= 0) return NULL;
        received += n;
    }
    buffer[received] = 0;
    return cJSON_Parse(buffer);
}

static cJSON *status_json(void)
{
    plant_status_t s; plant_get_status(&s);
    plant_settings_t cfg; plant_get_settings(&cfg);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "time_synced", time(NULL) > 1700000000);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON *soil = cJSON_AddObjectToObject(root, "soil");
    cJSON_AddNumberToObject(soil, "percent", s.soil_percent);
    cJSON_AddNumberToObject(soil, "raw", s.soil_raw);
    cJSON_AddBoolToObject(soil, "valid", s.soil_valid);
    cJSON_AddStringToObject(soil, "state", !s.soil_valid ? "fault" : s.soil_percent < cfg.soil_low ? "dry" : s.soil_percent >= cfg.soil_target ? "wet" : "normal");
    cJSON *pump = cJSON_AddObjectToObject(root, "pump");
    cJSON_AddBoolToObject(pump, "running", s.pump_running);
    cJSON_AddNumberToObject(pump, "remaining_ms", s.remaining_ms);
    cJSON_AddStringToObject(pump, "mode", s.mode);
    cJSON *tank = cJSON_AddObjectToObject(root, "tank");
    cJSON_AddBoolToObject(tank, "enabled", true);
    cJSON_AddBoolToObject(tank, "lock_enabled", false);
    cJSON_AddBoolToObject(tank, "valid", s.water_valid);
    cJSON_AddNumberToObject(tank, "raw", s.water_raw);
    cJSON_AddNumberToObject(tank, "percent", s.water_percent);
    cJSON_AddBoolToObject(tank, "empty", s.tank_empty);
    cJSON *aut = cJSON_AddObjectToObject(root, "auto");
    cJSON_AddBoolToObject(aut, "enabled", cfg.auto_enabled);
    cJSON_AddNumberToObject(aut, "today_count", s.today_count);
    cJSON_AddNumberToObject(aut, "max_daily", cfg.max_daily);
    cJSON_AddNumberToObject(aut, "today_seconds", s.today_seconds);
    cJSON_AddNumberToObject(aut, "last_time", s.last_auto_time);
    cJSON_AddNumberToObject(aut, "next_time", s.next_auto_time);
    cJSON_AddNumberToObject(aut, "next_seconds", s.next_auto_seconds);
    cJSON_AddStringToObject(aut, "wait_reason", s.auto_wait);
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", net_connected());
    cJSON_AddBoolToObject(wifi, "setup_mode", net_ap_mode());
    cJSON_AddStringToObject(wifi, "ssid", net_ssid());
    cJSON_AddStringToObject(wifi, "ip", net_ip());
    cJSON_AddNumberToObject(wifi, "rssi", net_rssi());
    cJSON_AddNumberToObject(root, "uptime", s.uptime_ms / 1000);
    cJSON_AddStringToObject(root, "alert", s.alert);
    cJSON_AddStringToObject(root, "name", cfg.name);
    cJSON_AddNumberToObject(root, "min_soil", s.min_soil);
    cJSON_AddNumberToObject(root, "max_soil", s.max_soil);
    return root;
}

static esp_err_t status_get(httpd_req_t *req) { return send_json(req, status_json()); }

static esp_err_t sync_time(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(body, "timestamp");
    if (!cJSON_IsNumber(timestamp) || timestamp->valuedouble < 1700000000 || timestamp->valuedouble > 4102444800) {
        cJSON_Delete(body);
        return error(req, 400, "INVALID_TIME", "时间无效");
    }
    if (time(NULL) <= 1700000000) {
        struct timeval now = {.tv_sec = (time_t)timestamp->valuedouble};
        settimeofday(&now, NULL);
    }
    cJSON_Delete(body);
    return send_json(req, status_json());
}

static cJSON *settings_json(void)
{
    plant_settings_t s; plant_get_settings(&s);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "auto_enabled", s.auto_enabled);
    cJSON_AddNumberToObject(root, "soil_low", s.soil_low);
    cJSON_AddNumberToObject(root, "soil_target", s.soil_target);
    cJSON_AddNumberToObject(root, "pump_seconds", s.pump_seconds);
    cJSON_AddNumberToObject(root, "soak_seconds", s.soak_seconds);
    cJSON_AddNumberToObject(root, "min_interval", s.min_interval);
    cJSON_AddNumberToObject(root, "max_daily", s.max_daily);
    cJSON_AddNumberToObject(root, "dry_adc", s.dry_adc);
    cJSON_AddNumberToObject(root, "wet_adc", s.wet_adc);
    cJSON_AddNumberToObject(root, "brightness", s.brightness);
    cJSON_AddBoolToObject(root, "brightness_supported", false);
    cJSON_AddNumberToObject(root, "water_dry_adc", s.water_dry_adc);
    cJSON_AddNumberToObject(root, "water_wet_adc", s.water_wet_adc);
    cJSON_AddStringToObject(root, "name", s.name);
    return root;
}

static esp_err_t settings_get(httpd_req_t *req) { return send_json(req, settings_json()); }

static bool int_field(cJSON *json, const char *name, int *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!item) return true;
    if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint) return false;
    *out = item->valueint;
    return true;
}

static esp_err_t settings_put(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    if (!cJSON_IsObject(body)) { cJSON_Delete(body); return error(req, 400, "INVALID_JSON", "无效的设置内容"); }
    plant_settings_t s; plant_get_settings(&s);
    bool valid = int_field(body, "soil_low", &s.soil_low) && int_field(body, "soil_target", &s.soil_target) &&
        int_field(body, "pump_seconds", &s.pump_seconds) && int_field(body, "soak_seconds", &s.soak_seconds) &&
        int_field(body, "min_interval", &s.min_interval) && int_field(body, "max_daily", &s.max_daily) &&
        int_field(body, "brightness", &s.brightness);
    cJSON *item = cJSON_GetObjectItemCaseSensitive(body, "auto_enabled");
    if (item) { if (!cJSON_IsBool(item)) valid = false; else s.auto_enabled = cJSON_IsTrue(item); }
    item = cJSON_GetObjectItemCaseSensitive(body, "name");
    if (item) {
        if (!cJSON_IsString(item) || !item->valuestring[0] || strlen(item->valuestring) >= sizeof(s.name)) valid = false;
        else strcpy(s.name, item->valuestring);
    }
    cJSON_Delete(body);
    if (!valid || plant_set_settings(&s) != ESP_OK) return error(req, 400, "INVALID_SETTINGS", "参数范围或关系无效");
    return send_json(req, settings_json());
}

static esp_err_t pump_start(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(body, "duration");
    if (!cJSON_IsNumber(duration) || duration->valuedouble != duration->valueint || duration->valueint < 1 || duration->valueint > 10) {
        cJSON_Delete(body); return error(req, 400, "INVALID_DURATION", "浇水时长须为 1–10 秒");
    }
    int seconds = duration->valueint;
    cJSON_Delete(body);
    esp_err_t e = plant_request_pump(seconds, false);
    if (e != ESP_OK) return error(req, 409, plant_error_name(e), "水泵正在运行或请求未满足运行条件");
    return send_json(req, status_json());
}

static esp_err_t pump_stop(httpd_req_t *req) { plant_stop_pump(); return send_json(req, status_json()); }
static esp_err_t calibrate_dry(httpd_req_t *req) { return plant_calibrate(false) == ESP_OK ? send_json(req, settings_json()) : error(req, 500, "CALIBRATION_FAILED", "干燥校准失败"); }
static esp_err_t calibrate_wet(httpd_req_t *req) { return plant_calibrate(true) == ESP_OK ? send_json(req, settings_json()) : error(req, 500, "CALIBRATION_FAILED", "湿润校准失败"); }

static esp_err_t water_dry(httpd_req_t *req) { return plant_calibrate_water(false) == ESP_OK ? send_json(req, settings_json()) : error(req, 500, "CALIBRATION_FAILED", "水位空水校准失败"); }
static esp_err_t water_wet(httpd_req_t *req) { return plant_calibrate_water(true) == ESP_OK ? send_json(req, settings_json()) : error(req, 500, "CALIBRATION_FAILED", "水位满水校准失败"); }

static esp_err_t history_chunk(const char *data, size_t size, void *context)
{
    return httpd_resp_send_chunk(context, data, size);
}

static esp_err_t history_get(httpd_req_t *req)
{
    char query[64], value[16]; int limit = 400, hours = 24;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "limit", value, sizeof(value)) == ESP_OK) limit = atoi(value);
        if (httpd_query_key_value(query, "hours", value, sizeof(value)) == ESP_OK) hours = atoi(value);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t result = store_history_send(limit, hours, history_chunk, req);
    if (result != ESP_OK) return result;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    char *json = net_scan_json();
    if (!json) return error(req, 503, "WIFI_SCAN_BUSY", "扫描暂不可用，请等待连接结束后重试");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t e = httpd_resp_sendstr(req, json);
    free(json);
    return e;
}

static esp_err_t wifi_put(httpd_req_t *req)
{
    cJSON *body = read_body(req);
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(body, "password");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password)) { cJSON_Delete(body); return error(req, 400, "INVALID_WIFI", "请输入 Wi-Fi 名称和密码"); }
    esp_err_t e = net_save_credentials(ssid->valuestring, password->valuestring);
    cJSON_Delete(body);
    if (e != ESP_OK) return error(req, 400, "INVALID_WIFI", "无法保存 Wi-Fi 配置");
    cJSON *result = cJSON_CreateObject(); cJSON_AddBoolToObject(result, "ok", true);
    return send_json(req, result);
}

static esp_err_t asset(httpd_req_t *req)
{
    const char *start, *end, *type;
    if (!strcmp(req->uri, "/styles.css")) { start = styles_css_start; end = styles_css_end; type = "text/css"; }
    else if (!strcmp(req->uri, "/app.js")) { start = app_js_start; end = app_js_end; type = "application/javascript"; }
    else { start = index_html_start; end = index_html_end; type = "text/html"; }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start, end - start);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return ESP_OK;
    }
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};
    return httpd_ws_recv_frame(req, &frame, 0);
}

static void broadcast_work(void *arg)
{
    char *json = arg;
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)json, .len = strlen(json)};
    int clients[WEB_MAX_CLIENTS];
    size_t count = WEB_MAX_CLIENTS;
    if (httpd_get_client_list(server, &count, clients) == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            if (httpd_ws_get_fd_info(server, clients[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
                httpd_ws_send_frame_async(server, clients[i], &frame);
        }
    }
    free(json);
}

void web_broadcast(void)
{
    if (!server) return;
    cJSON *root = status_json();
    cJSON_AddStringToObject(root, "type", "telemetry");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json && httpd_queue_work(server, broadcast_work, json) != ESP_OK) free(json);
}

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = WEB_MAX_CLIENTS;
    cfg.max_uri_handlers = 24;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    const httpd_uri_t routes[] = {
        {.uri="/", .method=HTTP_GET, .handler=asset},
        {.uri="/styles.css", .method=HTTP_GET, .handler=asset},
        {.uri="/app.js", .method=HTTP_GET, .handler=asset},
        {.uri="/api/v1/status", .method=HTTP_GET, .handler=status_get},
        {.uri="/api/v1/time", .method=HTTP_POST, .handler=sync_time},
        {.uri="/api/v1/settings", .method=HTTP_GET, .handler=settings_get},
        {.uri="/api/v1/settings", .method=HTTP_PUT, .handler=settings_put},
        {.uri="/api/v1/pump/start", .method=HTTP_POST, .handler=pump_start},
        {.uri="/api/v1/pump/stop", .method=HTTP_POST, .handler=pump_stop},
        {.uri="/api/v1/calibration/dry", .method=HTTP_POST, .handler=calibrate_dry},
        {.uri="/api/v1/calibration/wet", .method=HTTP_POST, .handler=calibrate_wet},
        {.uri="/api/v1/calibration/water/dry", .method=HTTP_POST, .handler=water_dry},
        {.uri="/api/v1/calibration/water/wet", .method=HTTP_POST, .handler=water_wet},
        {.uri="/api/v1/history", .method=HTTP_GET, .handler=history_get},
        {.uri="/api/v1/wifi/scan", .method=HTTP_GET, .handler=scan_get},
        {.uri="/api/v1/wifi", .method=HTTP_PUT, .handler=wifi_put},
        {.uri="/ws", .method=HTTP_GET, .handler=ws_handler, .is_websocket=true}
    };
    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
}
