#include "plant.h"
#include "bsp/esp32_s3_lcd_ev_board.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"

LV_FONT_DECLARE(ui_font_20);
LV_FONT_DECLARE(ui_font_28);
#define BG 0x0A1014
#define PANEL 0x162229
#define INK 0xEEF4F2
#define MUTED 0xADC0BA
#define GREEN 0x70E6AA
#define AMBER 0xFFD078
#define RED 0xFF8C89
#define BLUE 0x91D3F8
#define POINTS 96

enum { OVERVIEW, HISTORY, WATERING, AUTOMATION, SENSOR, NETWORK };
static int page, history_hours = 24;
static bool history_dirty = true;
static int64_t last_update, last_history;
static lv_obj_t *content, *network_state, *clock_label, *feedback, *nav[6];
static lv_obj_t *soil_value, *soil_state, *plan_value, *last_auto, *water_value, *pump_value, *auto_value;
static lv_obj_t *chart, *chart_summary, *chart_start, *chart_end;
static lv_chart_series_t *chart_series;
static lv_obj_t *auto_switch, *fields[6], *soil_raw, *soil_calibration, *water_raw, *water_calibration;
static lv_obj_t *network_name, *network_address, *network_detail, *network_list, *scan_button;
static lv_obj_t *network_editor, *ssid_field, *password_field, *keyboard, *editor_status, *connect_button;
static net_network_t nearby[16];
static int nearby_count;
static bool scan_requested, scan_busy, connect_requested, editor_submitted, editor_feedback_active;
static char editor_feedback_previous[192];
static lv_color_t editor_feedback_color;
static char pending_ssid[33], pending_password[64], submitted_ssid[33];
static char scan_hint[128] = "点击扫描，查找附近的 2.4 GHz 网络";
static unsigned scan_hint_color = MUTED;
static int64_t feedback_until;
static int fields_before[6];
static bool auto_before;
static const char *page_names[] = {"植物概览", "历史曲线", "手动浇水", "自动化", "传感器", "网络"};

static lv_color_t color(unsigned value) { return lv_color_hex(value); }
static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width, const char *text,
                       const lv_font_t *font, unsigned ink)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, width);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, color(ink), 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    return o;
}
static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                        unsigned ground, lv_event_cb_t callback, intptr_t data)
{
    lv_obj_t *o = lv_button_create(parent);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, color(ground), 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, color(ground == GREEN ? 0x52C88E : ground == RED ? 0xF06F6F : 0x344D45), LV_STATE_PRESSED);
    lv_obj_t *t = label(o, 0, 0, w - 12, text, &ui_font_20, ground == GREEN || ground == RED ? BG : INK);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(t);
    if (callback) lv_obj_add_event_cb(o, callback, LV_EVENT_CLICKED, (void *)data);
    return o;
}
static void message(const char *text, bool success)
{
    lv_label_set_text(feedback, text);
    lv_obj_set_style_text_color(feedback, color(success ? GREEN : RED), 0);
    feedback_until = esp_timer_get_time() + 5000000;
    if (network_editor) {
        if (!editor_feedback_active) {
            snprintf(editor_feedback_previous, sizeof(editor_feedback_previous), "%s", lv_label_get_text(editor_status));
            editor_feedback_color = lv_obj_get_style_text_color(editor_status, 0);
        }
        editor_feedback_active = true;
        lv_label_set_text(editor_status, text);
        lv_obj_set_style_text_color(editor_status, color(success ? GREEN : RED), 0);
    }
}
static void pump_event(lv_event_t *e)
{
    int duration = (intptr_t)lv_event_get_user_data(e);
    if (!duration) {
        plant_stop_pump();
        plant_status_t s; plant_get_status(&s);
        message(s.pump_running ? "停止失败，请断开水泵电源并检查驱动" : "水泵已停止", !s.pump_running);
    } else {
        esp_err_t result = plant_request_pump(duration, false);
        message(result == ESP_OK ? "已开始浇水，到时自动停止" : "启动失败，请检查驱动或当前水泵状态", result == ESP_OK);
    }
    last_update = 0;
}
static void format_time(int64_t value, char *out, size_t size)
{
    if (value <= 1700000000) { snprintf(out, size, "尚无记录"); return; }
    time_t t = value; struct tm tm; localtime_r(&t, &tm);
    strftime(out, size, "%m-%d %H:%M", &tm);
}
static void calibrate(lv_event_t *e)
{
    int kind = (intptr_t)lv_event_get_user_data(e);
    esp_err_t result = kind < 2 ? plant_calibrate(kind == 1) : plant_calibrate_water(kind == 3);
    message(result == ESP_OK ? "校准值已保存，请完成另一端校准" : "采集失败，请检查 ADS1115 和传感器接线", result == ESP_OK);
    last_update = 0;
}
static void history_range(lv_event_t *e)
{
    history_hours = (intptr_t)lv_event_get_user_data(e);
    history_dirty = true;
    lv_label_set_text(chart_summary, "正在读取记录...");
}
static void spin_event(lv_event_t *e)
{
    intptr_t id = (intptr_t)lv_event_get_user_data(e);
    int index = id / 2;
    if (id % 2) lv_spinbox_increment(fields[index]); else lv_spinbox_decrement(fields[index]);
}
static void save_auto(lv_event_t *e)
{
    (void)e;
    plant_settings_t next; plant_get_settings(&next);
    // Only fields edited here replace current settings; preserve changes made on the web.
    int *targets[] = {&next.soil_low, &next.soil_target, &next.pump_seconds,
                      &next.soak_seconds, &next.min_interval, &next.max_daily};
    for (int i = 0; i < 6; i++) {
        int value = lv_spinbox_get_value(fields[i]);
        if (value != fields_before[i]) *targets[i] = value;
    }
    bool enabled = lv_obj_has_state(auto_switch, LV_STATE_CHECKED);
    if (enabled != auto_before) next.auto_enabled = enabled;
    esp_err_t result = plant_set_settings(&next);
    if (result == ESP_OK) {
        for (int i = 0; i < 6; i++) fields_before[i] = lv_spinbox_get_value(fields[i]);
        auto_before = enabled;
    }
    message(result == ESP_OK ? "自动化设置已保存" : "设置无效：目标湿度须比启动阈值至少高 5%", result == ESP_OK);
    last_update = 0;
}
static void field(int index, int x, int y, const char *title, int value, int min, int max, int digits)
{
    label(content, x, y, 240, title, &ui_font_20, MUTED);
    button(content, x, y + 34, 48, 48, "-", PANEL, spin_event, index * 2);
    fields[index] = lv_spinbox_create(content);
    lv_obj_set_pos(fields[index], x + 56, y + 34); lv_obj_set_size(fields[index], 126, 48);
    lv_spinbox_set_range(fields[index], min, max);
    lv_spinbox_set_digit_format(fields[index], digits, 0);
    lv_spinbox_set_step(fields[index], 1); lv_spinbox_set_rollover(fields[index], false);
    lv_spinbox_set_value(fields[index], value);
    lv_obj_set_style_text_font(fields[index], &ui_font_28, 0);
    lv_obj_set_style_text_color(fields[index], color(INK), 0);
    lv_obj_set_style_bg_color(fields[index], color(PANEL), 0);
    lv_obj_set_style_border_width(fields[index], 0, 0);
    button(content, x + 190, y + 34, 48, 48, "+", PANEL, spin_event, index * 2 + 1);
    fields_before[index] = value;
}
static void close_network_editor(lv_event_t *e)
{
    (void)e;
    if (network_editor) lv_obj_delete(network_editor);
    network_editor = NULL;
    ssid_field = password_field = keyboard = editor_status = connect_button = NULL;
    editor_submitted = editor_feedback_active = false;
    last_update = 0;
}
static void submit_network(lv_event_t *e)
{
    (void)e;
    if (connect_requested || scan_busy) return;
    const char *ssid = lv_textarea_get_text(ssid_field);
    const char *password = lv_textarea_get_text(password_field);
    if (!ssid[0] || strlen(ssid) > 32 || strlen(password) > 63) {
        lv_label_set_text(editor_status, "网络名不能为空且最多 32 字节，密码最多 63 字节");
        lv_obj_set_style_text_color(editor_status, color(RED), 0);
        return;
    }
    strcpy(pending_ssid, ssid);
    strcpy(pending_password, password);
    strcpy(submitted_ssid, ssid);
    connect_requested = true;
    editor_submitted = false;
    lv_obj_add_state(connect_button, LV_STATE_DISABLED);
    lv_label_set_text(editor_status, "正在提交连接，请稍候...");
    lv_obj_set_style_text_color(editor_status, color(AMBER), 0);
}
static void keyboard_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) submit_network(e);
    else if (lv_event_get_code(e) == LV_EVENT_CANCEL) close_network_editor(e);
}
static void focus_network_field(lv_event_t *e)
{
    lv_keyboard_set_textarea(keyboard, lv_event_get_target_obj(e));
}
static void show_password(lv_event_t *e)
{
    (void)e;
    bool hidden = lv_textarea_get_password_mode(password_field);
    lv_textarea_set_password_mode(password_field, !hidden);
    lv_obj_t *text = lv_obj_get_child(lv_event_get_target_obj(e), 0);
    lv_label_set_text(text, hidden ? "隐藏密码" : "显示密码");
}
static lv_obj_t *network_input(int x, int y, int width)
{
    lv_obj_t *input = lv_textarea_create(network_editor);
    lv_obj_set_pos(input, x, y); lv_obj_set_size(input, width, 46);
    lv_textarea_set_one_line(input, true);
    lv_obj_set_style_bg_color(input, color(PANEL), 0);
    lv_obj_set_style_text_color(input, color(INK), 0);
    lv_obj_set_style_text_color(input, color(MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(input, &ui_font_20, 0);
    lv_obj_set_style_border_color(input, color(0x34463F), 0);
    lv_obj_set_style_border_color(input, color(BLUE), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(input, 1, 0);
    lv_obj_set_style_radius(input, 8, 0);
    lv_obj_set_style_pad_hor(input, 10, 0);
    lv_obj_set_style_pad_ver(input, 3, 0);
    lv_obj_set_height(input, 46);
    lv_obj_add_event_cb(input, focus_network_field, LV_EVENT_FOCUSED, NULL);
    return input;
}
static void edit_network(lv_event_t *e)
{
    int index = (intptr_t)lv_event_get_user_data(e);
    if (scan_busy || connect_requested) return;
    close_network_editor(NULL);
    network_editor = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(network_editor, 0, 54); lv_obj_set_size(network_editor, 800, 426);
    lv_obj_set_style_bg_color(network_editor, color(BG), 0);
    lv_obj_set_style_border_width(network_editor, 0, 0);
    lv_obj_set_style_pad_all(network_editor, 0, 0);
    lv_obj_set_style_radius(network_editor, 0, 0);
    lv_obj_remove_flag(network_editor, LV_OBJ_FLAG_SCROLLABLE);
    label(network_editor, 24, 8, 600, "连接 Wi-Fi", &ui_font_28, INK);
    button(network_editor, 660, 6, 116, 44, "返回", PANEL, close_network_editor, 0);
    label(network_editor, 24, 64, 76, "网络名", &ui_font_20, MUTED);
    ssid_field = network_input(106, 54, 670);
    lv_textarea_set_max_length(ssid_field, 32);
    lv_textarea_set_placeholder_text(ssid_field, "输入网络名，中文名称请从扫描列表选择");
    label(network_editor, 24, 119, 76, "密码", &ui_font_20, MUTED);
    password_field = network_input(106, 109, 430);
    lv_textarea_set_max_length(password_field, 63);
    lv_textarea_set_password_mode(password_field, true);
    lv_textarea_set_password_show_time(password_field, 0);
    lv_textarea_set_placeholder_text(password_field, "输入密码；开放网络可留空");
    button(network_editor, 548, 109, 100, 46, "显示密码", PANEL, show_password, 0);
    connect_button = button(network_editor, 660, 109, 116, 46, "连接网络", GREEN, submit_network, 0);
    editor_status = label(network_editor, 24, 170, 752, "网络设置会保存，下次开机自动连接。", &ui_font_20, MUTED);
    keyboard = lv_keyboard_create(network_editor);
    lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(keyboard, 0, 222); lv_obj_set_size(keyboard, 800, 198);
    // The keyboard needs Montserrat's symbol glyphs (delete, enter and arrows).
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, color(INK), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, color(PANEL), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, color(BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard, color(0x344D45), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboard, color(INK), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(keyboard, color(GREEN), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(keyboard, color(BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_ALL, NULL);
    if (index >= 0 && index < nearby_count) {
        lv_textarea_set_text(ssid_field, nearby[index].ssid);
        // Keep scanned names intact, including Chinese and 32-byte SSIDs.
        lv_obj_remove_flag(ssid_field, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_keyboard_set_textarea(keyboard, password_field);
        lv_obj_add_state(password_field, LV_STATE_FOCUSED);
        if (nearby[index].open) lv_label_set_text(editor_status, "这是开放网络，可以不填写密码直接连接。");
    } else {
        lv_keyboard_set_textarea(keyboard, ssid_field);
        lv_obj_add_state(ssid_field, LV_STATE_FOCUSED);
    }
}
static void scan_networks(lv_event_t *e)
{
    (void)e;
    if (scan_busy || connect_requested) return;
    scan_requested = true;
    scan_busy = true;
    strcpy(scan_hint, "正在扫描，请稍候...");
    scan_hint_color = AMBER;
    lv_obj_add_state(scan_button, LV_STATE_DISABLED);
    lv_obj_clean(network_list);
    label(network_list, 12, 12, 350, scan_hint, &ui_font_20, AMBER);
}
static void populate_networks(void)
{
    lv_obj_clean(network_list);
    if (!nearby_count) {
        label(network_list, 12, 12, 350, scan_hint, &ui_font_20, scan_hint_color);
        return;
    }
    for (int i = 0; i < nearby_count; i++) {
        lv_obj_t *row = button(network_list, 0, i * 80, 372, 72, "", PANEL, edit_network, i);
        lv_obj_t *name = label(row, 12, 6, 348, nearby[i].ssid, &ui_font_20, INK);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_height(name, 38);
        char text[96];
        snprintf(text, sizeof(text), "%s · %s · %d dBm", nearby[i].open ? "开放网络" : "需要密码",
                 nearby[i].rssi >= -60 ? "信号强" : nearby[i].rssi >= -75 ? "信号中等" : "信号弱", nearby[i].rssi);
        label(row, 12, 39, 348, text, &ui_font_20, MUTED);
    }
}
// Called by the existing UI task. Network operations never hold the LVGL lock.
static void network_work(void)
{
    bool scan = false, connect = false;
    char ssid[33] = "", password[64] = "";
    if (!bsp_display_lock(100)) return;
    scan = scan_requested; scan_requested = false;
    connect = connect_requested;
    if (connect) { strcpy(ssid, pending_ssid); strcpy(password, pending_password); }
    bsp_display_unlock();
    if (!scan && !connect) return;
    net_network_t results[16]; int count = 0;
    esp_err_t result = scan ? net_scan(results, 16, &count) : net_save_credentials(ssid, password);
    memset(password, 0, sizeof(password));
    // Once an operation finishes, apply its result even if the page changed meanwhile.
    bsp_display_lock(0);
    if (scan) {
        scan_busy = false;
        nearby_count = result == ESP_OK ? count : 0;
        if (nearby_count) memcpy(nearby, results, count * sizeof(results[0]));
        snprintf(scan_hint, sizeof(scan_hint), "%s", result != ESP_OK
            ? "扫描暂不可用，请等连接结束后重试"
            : "未找到网络，请靠近路由器后重新扫描");
        scan_hint_color = result != ESP_OK ? RED : MUTED;
        if (page == NETWORK) { lv_obj_remove_state(scan_button, LV_STATE_DISABLED); populate_networks(); }
    } else {
        connect_requested = false;
        memset(pending_password, 0, sizeof(pending_password));
        if (network_editor) {
            lv_obj_remove_state(connect_button, LV_STATE_DISABLED);
            editor_submitted = result == ESP_OK;
            lv_label_set_text(editor_status, result == ESP_OK ? "正在连接，请稍候..." : "无法启动连接，请稍后点击连接网络重试");
            lv_obj_set_style_text_color(editor_status, color(result == ESP_OK ? AMBER : RED), 0);
        }
    }
    last_update = 0;
    bsp_display_unlock();
}
static void build_page(int selected)
{
    close_network_editor(NULL);
    page = selected;
    lv_obj_clean(content);
    chart = NULL;
    for (int i = 0; i < 6; i++) lv_obj_set_style_bg_color(nav[i], color(i == page ? 0x2B493B : PANEL), 0);
    if (page == OVERVIEW) {
        label(content, 24, 12, 340, "土壤湿度", &ui_font_28, INK);
        soil_value = label(content, 24, 62, 330, "-- %", &lv_font_montserrat_48, GREEN);
        soil_state = label(content, 24, 126, 340, "等待读取与校准", &ui_font_20, AMBER);
        pump_value = label(content, 24, 178, 340, "水泵：已停止", &ui_font_20, AMBER);
        water_value = label(content, 24, 222, 360, "水位：等待读取", &ui_font_20, BLUE);
        label(content, 24, 256, 360, "水位仅供查看，缺水锁定已关闭", &ui_font_20, MUTED);
        auto_value = label(content, 414, 12, 350, "自动浇水：关闭", &ui_font_28, AMBER);
        label(content, 414, 60, 350, "距离下次允许浇水", &ui_font_20, MUTED);
        plan_value = label(content, 414, 100, 350, "--:--:--", &ui_font_28, INK);
        last_auto = label(content, 414, 158, 350, "上次自动浇水：尚无记录", &ui_font_20, MUTED);
        label(content, 414, 203, 350, "到时仍需土壤偏干才会启动", &ui_font_20, MUTED);
        button(content, 414, 251, 350, 54, "手动浇水 3 秒", GREEN, pump_event, 3);
    } else if (page == HISTORY) {
        chart_summary = label(content, 24, 12, 350, "正在读取记录...", &ui_font_20, MUTED);
        button(content, 392, 8, 116, 44, "24 小时", PANEL, history_range, 24);
        button(content, 516, 8, 116, 44, "7 天", PANEL, history_range, 168);
        button(content, 640, 8, 116, 44, "30 天", PANEL, history_range, 720);
        label(content, 12, 66, 44, "100", &ui_font_20, MUTED);
        label(content, 12, 160, 44, "50", &ui_font_20, MUTED);
        label(content, 12, 260, 44, "0", &ui_font_20, MUTED);
        chart = lv_chart_create(content);
        lv_obj_set_pos(chart, 66, 70); lv_obj_set_size(chart, 688, 214);
        lv_obj_set_style_bg_color(chart, color(BG), 0);
        lv_obj_set_style_border_color(chart, color(0x34463F), 0);
        lv_obj_set_style_line_color(chart, color(0x34463F), LV_PART_MAIN);
        lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
        lv_obj_set_style_size(chart, 4, 4, LV_PART_INDICATOR);
        lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        lv_chart_set_div_line_count(chart, 3, 5);
        lv_chart_set_point_count(chart, POINTS);
        chart_series = lv_chart_add_series(chart, color(GREEN), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_all_value(chart, chart_series, LV_CHART_POINT_NONE);
        chart_start = label(content, 66, 294, 250, "", &ui_font_20, MUTED);
        chart_end = label(content, 504, 294, 250, "", &ui_font_20, MUTED);
        lv_obj_set_style_text_align(chart_end, LV_TEXT_ALIGN_RIGHT, 0);
        history_dirty = true;
    } else if (page == WATERING) {
        label(content, 24, 12, 720, "手动浇水", &ui_font_28, INK);
        pump_value = label(content, 24, 62, 720, "水泵：等待状态", &ui_font_28, AMBER);
        label(content, 24, 112, 720, "每次最长 10 秒，设备会自动停止。请确认水箱有水。", &ui_font_20, MUTED);
        button(content, 24, 168, 232, 76, "浇水 3 秒", GREEN, pump_event, 3);
        button(content, 280, 168, 232, 76, "浇水 5 秒", GREEN, pump_event, 5);
        button(content, 536, 168, 232, 76, "浇水 10 秒", GREEN, pump_event, 10);
        label(content, 24, 275, 720, "随时可点右上角“停止水泵”。水位不会限制浇水。", &ui_font_20, MUTED);
    } else if (page == AUTOMATION) {
        plant_settings_t s; plant_get_settings(&s);
        label(content, 24, 6, 580, "自动浇水", &ui_font_28, INK);
        auto_switch = lv_switch_create(content);
        lv_obj_set_pos(auto_switch, 688, 8); lv_obj_set_size(auto_switch, 76, 38);
        lv_obj_set_style_bg_color(auto_switch, color(GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (s.auto_enabled) lv_obj_add_state(auto_switch, LV_STATE_CHECKED);
        auto_before = s.auto_enabled;
        field(0, 24, 66, "启动湿度 (%)", s.soil_low, 5, 80, 2);
        field(1, 280, 66, "目标湿度 (%)", s.soil_target, 10, 95, 2);
        field(2, 536, 66, "每次浇水 (秒)", s.pump_seconds, 1, 10, 2);
        field(3, 24, 170, "渗透等待 (秒)", s.soak_seconds, 10, 600, 3);
        field(4, 280, 170, "最短间隔 (分)", s.min_interval, 5, 1440, 4);
        field(5, 536, 170, "每日上限 (次)", s.max_daily, 1, 20, 2);
        label(content, 24, 278, 478, "修改后保存才生效。先完成土壤校准。", &ui_font_20, MUTED);
        button(content, 536, 270, 232, 48, "保存设置", GREEN, save_auto, 0);
    } else if (page == SENSOR) {
        label(content, 24, 8, 350, "土壤传感器", &ui_font_28, INK);
        soil_raw = label(content, 24, 58, 350, "等待采集", &ui_font_20, MUTED);
        soil_calibration = label(content, 24, 92, 350, "干燥 / 湿润：未校准", &ui_font_20, MUTED);
        label(content, 24, 128, 350, "保持探头稳定，再记录两端读数", &ui_font_20, MUTED);
        button(content, 24, 176, 166, 52, "记录干燥", PANEL, calibrate, 0);
        button(content, 202, 176, 166, 52, "记录湿润", PANEL, calibrate, 1);
        label(content, 414, 8, 350, "水位传感器", &ui_font_28, INK);
        water_raw = label(content, 414, 58, 350, "等待采集", &ui_font_20, MUTED);
        water_calibration = label(content, 414, 92, 350, "空水 / 满水：未校准", &ui_font_20, MUTED);
        label(content, 414, 128, 350, "只浸入条纹区，不超过板上水位线", &ui_font_20, MUTED);
        button(content, 414, 176, 166, 52, "记录空水", PANEL, calibrate, 2);
        button(content, 592, 176, 166, 52, "记录满水", PANEL, calibrate, 3);
        label(content, 24, 264, 740, "水位为校准后的相对值，不能代表整个水箱的剩余容量。", &ui_font_20, BLUE);
    } else {
        label(content, 24, 8, 340, "网络与网页", &ui_font_28, INK);
        network_detail = label(content, 24, 55, 328, "等待网络状态", &ui_font_20, AMBER);
        network_name = label(content, 24, 108, 328, "", &ui_font_20, INK);
        lv_label_set_long_mode(network_name, LV_LABEL_LONG_DOT);
        lv_obj_set_height(network_name, 38);
        network_address = label(content, 24, 148, 340, "", &ui_font_28, BLUE);
        label(content, 24, 194, 328, "电脑或手机连接同一网络，\n打开上方地址访问网页。", &ui_font_20, MUTED);
        button(content, 24, 269, 328, 48, "手动输入网络名", PANEL, edit_network, -1);
        label(content, 380, 12, 250, "附近 Wi-Fi · 2.4 GHz", &ui_font_20, INK);
        scan_button = button(content, 652, 6, 124, 44, "扫描网络", PANEL, scan_networks, 0);
        if (scan_busy) lv_obj_add_state(scan_button, LV_STATE_DISABLED);
        network_list = lv_obj_create(content);
        lv_obj_set_pos(network_list, 380, 58); lv_obj_set_size(network_list, 396, 259);
        lv_obj_set_style_bg_opa(network_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(network_list, 0, 0);
        lv_obj_set_style_pad_all(network_list, 0, 0);
        lv_obj_set_scroll_dir(network_list, LV_DIR_VER);
        lv_obj_set_style_bg_color(network_list, color(MUTED), LV_PART_SCROLLBAR);
        populate_networks();
    }
    last_update = 0;
}
static void navigate(lv_event_t *e) { build_page((intptr_t)lv_event_get_user_data(e)); }

void screen_init(void)
{
    assert(bsp_display_start());
    bsp_display_lock(0);
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, color(BG), 0);
    lv_obj_set_style_text_font(root, &ui_font_20, 0);
    lv_obj_set_style_text_color(root, color(INK), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    label(root, 24, 12, 280, "SmartPlant 浇花控制台", &ui_font_20, INK);
    network_state = label(root, 320, 12, 200, "网络连接中", &ui_font_20, MUTED);
    clock_label = label(root, 526, 12, 106, "--:--", &ui_font_20, MUTED);
    button(root, 648, 6, 128, 40, "停止水泵", RED, pump_event, 0);
    content = lv_obj_create(root);
    lv_obj_set_pos(content, 0, 54); lv_obj_set_size(content, 800, 336);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    feedback = label(root, 24, 393, 750, "", &ui_font_20, MUTED);
    for (int i = 0; i < 6; i++) nav[i] = button(root, 16 + i * 130, 427, 120, 44, page_names[i], PANEL, navigate, i);
    build_page(OVERVIEW);
    bsp_display_unlock();
}

void screen_tick(void)
{
    network_work();
    int64_t now = esp_timer_get_time();
    // Read history outside the LVGL lock; touch and rendering continue while Flash is read.
    int requested_hours = history_hours;
    int values[POINTS], samples = -1;
    if (page == HISTORY && (history_dirty || now - last_history >= 60000000)) {
        history_dirty = false;
        samples = store_chart(requested_hours, values, POINTS);
        last_history = now;
    }
    if (now - last_update < 500000 && samples < 0) return;
    plant_status_t s; plant_get_status(&s);
    plant_settings_t cfg; plant_get_settings(&cfg);
    if (!bsp_display_lock(100)) return;
    last_update = now;
    time_t wall = time(NULL); struct tm tm; localtime_r(&wall, &tm);
    char text[192];
    if (wall > 1700000000) strftime(text, sizeof(text), "%H:%M", &tm); else strcpy(text, "未校时");
    lv_label_set_text(clock_label, text);
    lv_label_set_text(network_state, net_connected() ? "Wi-Fi 已连接" : net_ap_mode() ? "配网热点已开启" : "正在连接 Wi-Fi");
    lv_obj_set_style_text_color(network_state, color(net_connected() ? GREEN : AMBER), 0);
    if (feedback_until && now >= feedback_until) {
        lv_label_set_text(feedback, ""); feedback_until = 0;
        if (network_editor && editor_feedback_active) {
            lv_label_set_text(editor_status, editor_feedback_previous);
            lv_obj_set_style_text_color(editor_status, editor_feedback_color, 0);
        }
        editor_feedback_active = false;
    }
    if (page == OVERVIEW || page == WATERING) {
        if (s.pump_running) snprintf(text, sizeof(text), "正在浇水 · 剩余 %d 秒", (s.remaining_ms + 999) / 1000);
        else strcpy(text, "水泵已停止");
        lv_label_set_text(pump_value, text);
        lv_obj_set_style_text_color(pump_value, color(s.pump_running ? GREEN : AMBER), 0);
    }
    if (page == OVERVIEW) {
        if (s.soil_valid) snprintf(text, sizeof(text), "%d %%", s.soil_percent); else strcpy(text, "-- %");
        lv_label_set_text(soil_value, text);
        lv_obj_set_style_text_color(soil_value, color(!s.soil_valid ? RED : s.soil_percent < cfg.soil_low ? AMBER : GREEN), 0);
        lv_label_set_text(soil_state, !s.soil_valid ? "等待土壤校准或检查采集接线" : s.soil_percent < cfg.soil_low ? "土壤偏干" : "土壤湿度正常");
        lv_obj_set_style_text_color(soil_state, color(!s.soil_valid ? RED : s.soil_percent < cfg.soil_low ? AMBER : GREEN), 0);
        lv_label_set_text(auto_value, cfg.auto_enabled ? "自动浇水：开启" : "自动浇水：关闭");
        lv_obj_set_style_text_color(auto_value, color(cfg.auto_enabled ? GREEN : AMBER), 0);
        if (s.next_auto_seconds > 0) snprintf(text, sizeof(text), "%02d:%02d:%02d", s.next_auto_seconds / 3600, s.next_auto_seconds / 60 % 60, s.next_auto_seconds % 60);
        else strcpy(text, !strcmp(s.auto_wait, "disabled") ? "自动浇水已关闭" : !strcmp(s.auto_wait, "sensor_fault") ? "等待土壤校准" : !strcmp(s.auto_wait, "daily_limit") ? "今日次数已达上限" : !strcmp(s.auto_wait, "wet") ? "等待土壤变干" : "正在判断浇水条件");
        lv_label_set_text(plan_value, text);
        char stamp[32]; format_time(s.last_auto_time, stamp, sizeof(stamp));
        snprintf(text, sizeof(text), "上次自动浇水：%s", stamp); lv_label_set_text(last_auto, text);
        if (s.water_valid) snprintf(text, sizeof(text), "相对水位：%d %%", s.water_percent);
        else strcpy(text, s.water_raw < 0 ? "水位：采集失败" : "水位：尚未完成两端校准");
        lv_label_set_text(water_value, text);
        lv_obj_set_style_text_color(water_value, color(s.water_raw < 0 ? RED : s.water_valid ? BLUE : AMBER), 0);
    } else if (page == SENSOR) {
        if (s.soil_raw < 0) strcpy(text, "采集失败，请检查接线");
        else snprintf(text, sizeof(text), "ADC：%d  (0~4095)", s.soil_raw);
        lv_label_set_text(soil_raw, text);
        lv_obj_set_style_text_color(soil_raw, color(s.soil_raw < 0 ? RED : INK), 0);
        snprintf(text, sizeof(text), "干燥 %d / 湿润 %d", cfg.dry_adc, cfg.wet_adc); lv_label_set_text(soil_calibration, text);
        if (s.water_raw < 0) strcpy(text, "采集失败，请检查接线");
        else snprintf(text, sizeof(text), "ADC：%d  (0~4095)", s.water_raw);
        lv_label_set_text(water_raw, text);
        lv_obj_set_style_text_color(water_raw, color(s.water_raw < 0 ? RED : BLUE), 0);
        char dry[16], wet[16];
        if (cfg.water_dry_adc < 0) strcpy(dry, "未校准"); else snprintf(dry, sizeof(dry), "%d", cfg.water_dry_adc);
        if (cfg.water_wet_adc < 0) strcpy(wet, "未校准"); else snprintf(wet, sizeof(wet), "%d", cfg.water_wet_adc);
        snprintf(text, sizeof(text), "空水 %s / 满水 %s", dry, wet); lv_label_set_text(water_calibration, text);
    } else if (page == NETWORK) {
        lv_label_set_text(network_detail, net_status_text());
        lv_obj_set_style_text_color(network_detail, color(net_connected() ? GREEN : strstr(net_status_text(), "失败") || strstr(net_status_text(), "找不到") ? RED : AMBER), 0);
        snprintf(text, sizeof(text), "%s", net_ssid()[0] ? net_ssid() : "未选择网络"); lv_label_set_text(network_name, text);
        snprintf(text, sizeof(text), "%s%s", strcmp(net_ip(), "0.0.0.0") ? "http://" : "", strcmp(net_ip(), "0.0.0.0") ? net_ip() : "等待获取网页地址");
        lv_label_set_text(network_address, text);
        if (network_editor && editor_submitted && !editor_feedback_active) {
            bool success = net_connected() && !strcmp(net_ssid(), submitted_ssid);
            snprintf(text, sizeof(text), "%s", success ? "已连接并保存，点击返回查看网页地址" : net_status_text());
            lv_label_set_text(editor_status, text);
            lv_obj_set_style_text_color(editor_status, color(success ? GREEN : strstr(text, "失败") || strstr(text, "找不到") ? RED : AMBER), 0);
        }
    } else if (page == HISTORY && chart && samples >= 0 && history_hours == requested_hours) {
        for (int i = 0; i < POINTS; i++) lv_chart_set_value_by_id(chart, chart_series, i, values[i] < 0 ? LV_CHART_POINT_NONE : values[i]);
        lv_chart_refresh(chart);
        if (samples) snprintf(text, sizeof(text), "%s · %d 条记录", requested_hours == 24 ? "24 小时" : requested_hours == 168 ? "7 天" : "30 天", samples);
        else strcpy(text, wall <= 1700000000 ? "等待网络或网页校时" : "暂无有效湿度记录");
        lv_label_set_text(chart_summary, text);
        format_time(wall - requested_hours * 3600LL, text, sizeof(text)); lv_label_set_text(chart_start, text);
        format_time(wall, text, sizeof(text)); lv_label_set_text(chart_end, text);
    }
    if (!strcmp(s.alert, "PUMP_IO_FAULT")) {
        lv_label_set_text(feedback, "水泵停止失败，请断开水泵电源并检查 I2C 驱动");
        lv_obj_set_style_text_color(feedback, color(RED), 0);
        if (network_editor) {
            lv_label_set_text(editor_status, "水泵停止失败，请断开水泵电源并检查 I2C 驱动");
            lv_obj_set_style_text_color(editor_status, color(RED), 0);
        }
    }
    bsp_display_unlock();
}
