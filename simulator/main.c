#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "rf_ui.h"
LV_FONT_DECLARE(rf_font_debug_22);

static uint32_t preview_tick(void) { return (uint32_t)GetTickCount(); }

static uint32_t key_mask(HWND window)
{
    if (GetForegroundWindow() != window) return 0;
    const int keys[] = {VK_UP, VK_RIGHT, VK_LEFT, VK_RETURN, VK_DOWN};
    uint32_t mask = 0;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (GetAsyncKeyState(keys[i]) & 0x8000) mask |= 1U << i;
    }
    return mask;
}

static void scan_preview_keys(lv_timer_t *timer)
{
    static uint32_t previous;
    static bool previous_right;
    HWND window = (HWND)lv_timer_get_user_data(timer);
    uint32_t mask = key_mask(window);
    if (mask != previous) {
        previous = mask;
        rf_ui_set_keys(mask);
    }
    bool right = GetForegroundWindow() == window && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
    if (right && !previous_right) {
        lv_indev_t *pointer = lv_windows_acquire_pointer_indev(lv_display_get_default());
        if (pointer) {
            lv_point_t point;
            lv_indev_get_point(pointer, &point);
            rf_ui_signal_context_at(point.x, point.y);
        }
    }
    previous_right = right;
}

static void simulate_debug(lv_timer_t *timer)
{
    (void)timer;
    static bool enabled;
    if (rf_ui_take_debug_request()) { enabled = !enabled; rf_ui_set_debug_state(enabled ? RF_DEBUG_READY : RF_DEBUG_OFF); }
}

// PC-only radio model. Actual firmware uses RMT/CC1101; no RF is emitted here.
static void simulate_radio(lv_timer_t *timer)
{
    (void)timer;
    static rf_status_t model;
    static uint32_t changed, next_id = 1;
    static bool auto_receive;
    rf_command_t request;
    if (rf_ui_take_radio_action(&request)) {
        changed = lv_tick_get();
        switch (request.type) {
            case RF_CMD_RECEIVE: auto_receive = !request.frequency_hz; model.state = RF_LISTENING; model.frequency_hz = request.frequency_hz ? request.frequency_hz : 433920000; break;
            case RF_CMD_STOP: model.state = RF_IDLE; break;
            case RF_CMD_SAVE:
                if (model.state == RF_CAPTURED && model.library_count < RF_LIBRARY_LIMIT) {
                    rf_library_item_t *item = &model.library[model.library_count++];
                    item->id = next_id++; snprintf(item->code, sizeof(item->code), "%s", model.code);
                    item->frequency_hz = model.frequency_hz;
                    model.saved_id = item->id; model.state = auto_receive ? RF_LISTENING : RF_SAVED;
                }
                break;
            case RF_CMD_SEND: model.state = RF_SENDING; break;
            case RF_CMD_DELETE:
                for (unsigned i = 0; i < model.library_count; i++) if (model.library[i].id == request.id) {
                    memmove(&model.library[i], &model.library[i+1], (model.library_count-i-1)*sizeof(model.library[0])); model.library_count--; break;
                }
                model.state = RF_IDLE; break;
            case RF_CMD_MONITOR: model.state = RF_MONITORING; model.frequency_hz = request.frequency_hz ? request.frequency_hz : 433920000; break;
            default: break;
        }
        model.revision++;
    }
    if (model.state == RF_LISTENING && lv_tick_elaps(changed) >= 1200) {
        model.state = RF_CAPTURED; model.bits = 24; model.pulse_count = 50;
        snprintf(model.code, sizeof(model.code), "12AB01"); model.revision++;
    }
    if (model.state == RF_SENDING && lv_tick_elaps(changed) >= 600) { model.state = RF_SENT; model.revision++; }
    if (model.state == RF_MONITORING && lv_tick_elaps(changed) >= 300) {
        changed = lv_tick_get(); model.rssi_dbm = -80 + (changed / 300) % 15;
        model.occupancy = (changed / 300) % 60; model.revision++;
    }
    rf_ui_update_radio(&model);
}

static int save_preview(lv_display_t *display, const char *filename)
{
    lv_obj_t *screen = lv_screen_active();
    // Render the real shared UI for visual verification, without desktop capture.
    lv_draw_buf_t *image = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
    if (!image || image->header.w != 160 || image->header.h != 80) return 5;
    FILE *file = fopen(filename, "wb");
    if (!file) { lv_draw_buf_destroy(image); return 6; }
    BITMAPFILEHEADER header = {0};
    BITMAPINFOHEADER info = {0};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + 160 * 80 * 3;
    info.biSize = sizeof(info);
    info.biWidth = 160;
    info.biHeight = -80;
    info.biPlanes = 1;
    info.biBitCount = 24;
    bool written = fwrite(&header, sizeof(header), 1, file) == 1 &&
                   fwrite(&info, sizeof(info), 1, file) == 1;
    for (unsigned y = 0; y < 80 && written; y++) {
        written = fwrite(image->data + y * image->header.stride, 160 * 3, 1, file) == 1;
    }
    if (fclose(file)) written = false;
    lv_draw_buf_destroy(image);
    if (!written) return 7;
    lv_refr_now(display);
    return 0;
}

static lv_obj_t *visible_menu_label(lv_obj_t *viewport)
{
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_t *row = lv_obj_get_child(viewport, i);
        if (!lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) return lv_obj_get_child(row, 1);
    }
    return NULL;
}

static void finish_animation(void)
{
    Sleep(360);
    lv_timer_handler();
    lv_obj_update_layout(lv_screen_active());
}

static bool fits_inside(lv_obj_t *object, lv_obj_t *parent, int inset)
{
    lv_area_t bounds, container;
    lv_obj_get_coords(object, &bounds);
    lv_obj_get_coords(parent, &container);
    return bounds.x1 >= container.x1 + inset && bounds.y1 >= container.y1 + inset &&
           bounds.x2 <= container.x2 - inset && bounds.y2 <= container.y2 - inset;
}

static uint32_t test_clock;
static uint32_t test_tick(void) { return test_clock; }
static void advance_test_time(unsigned milliseconds)
{
    for (unsigned i = 0; i < milliseconds; i += 10) {
        test_clock += 10;
        lv_timer_handler();
    }
}

static int smoke_test(lv_display_t *display)
{
    for (uint32_t code = 33; code <= 126; code++) {
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(&rf_font_debug_22, &glyph, code, 0) || glyph.is_placeholder) return 79;
    }
    // Inspect the actual Windows framebuffer before snapshots or forced refreshes.
    lv_draw_buf_t *startup = lv_display_get_buf_active(display);
    if (!startup || !startup->data) return 11;
    const uint16_t *pixels = (const uint16_t *)startup->data;
    unsigned colored = 0;
    for (unsigned i = 0; i < 160 * 80; i++) if (pixels[i] != 0) colored++;
    if (colored < 160 * 80 / 2) return 12;
    if (lv_display_get_horizontal_resolution(display) != 160 ||
        lv_display_get_vertical_resolution(display) != 80) return 1;
    lv_obj_t *screen = lv_screen_active();
    if (lv_obj_get_child_count(screen) != 4) return 2;
    lv_obj_t *raw_label = lv_obj_get_child(screen, 3);
    rf_ui_set_battery_raw(-1);
    if (strcmp(lv_label_get_text(raw_label), "----")) return 80;
    rf_ui_set_battery_raw(0);
    if (strcmp(lv_label_get_text(raw_label), "0")) return 81;
    rf_ui_set_battery_raw(4095);
    if (strcmp(lv_label_get_text(raw_label), "4095")) return 82;
    lv_obj_update_layout(screen);
    lv_area_t raw_area, battery_area;
    lv_obj_get_coords(raw_label, &raw_area);
    lv_obj_get_coords(lv_obj_get_child(screen, 0), &battery_area);
    if (!fits_inside(raw_label, screen, 0) || raw_area.x2 >= battery_area.x1 ||
        raw_area.y2 >= 18) return 83;
    lv_obj_t *viewport = lv_obj_get_child(screen, 1);
    lv_obj_t *counter = lv_obj_get_child(screen, 2);
    lv_obj_t *battery = lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(screen, 0), 0), 0);
    if (strcmp(lv_label_get_text(battery), "3.85V")) return 13;
    rf_ui_set_battery_mv(0);
    if (strcmp(lv_label_get_text(battery), "--.--V")) return 14;
    lv_obj_update_layout(screen);
    if (!fits_inside(battery, lv_obj_get_parent(battery), 1)) return 18;
    rf_ui_set_battery_mv(4200);
    if (strcmp(lv_label_get_text(battery), "4.20V")) return 15;
    rf_ui_set_battery_mv(3850);
    const char *titles[] = {"接收", "发射", "干扰", "调试", "设置"};
    for (unsigned i = 0; i < 5; i++) {
        char expected[16], filename[32];
        snprintf(expected, sizeof(expected), "%u/5", i + 1);
        if (strcmp(lv_label_get_text(visible_menu_label(viewport)), titles[i]) ||
            strcmp(lv_label_get_text(counter), expected)) return 3;
        lv_obj_update_layout(screen);
        lv_obj_t *label = visible_menu_label(viewport);
        lv_obj_t *icon = lv_obj_get_child(lv_obj_get_parent(label), 0);
        if (!fits_inside(label, viewport, 0) || !fits_inside(icon, viewport, 0) ||
            !fits_inside(counter, screen, 0) ||
            !fits_inside(battery, lv_obj_get_parent(battery), 1)) return 19;
        snprintf(filename, sizeof(filename), "preview-%u.bmp", i + 1);
        int result = save_preview(display, filename);
        if (result) return result;
        rf_ui_set_keys(1U << 4); // Down once.
        rf_ui_set_keys(1U << 4); // Holding must not flip twice.
        rf_ui_set_keys(0);
        if (i == 0) {
            Sleep(70);
            lv_timer_handler();
            lv_obj_update_layout(screen);
            lv_obj_t *incoming = lv_obj_get_child(viewport, 1);
            if (lv_obj_get_y(incoming) <= 0 || lv_obj_get_y(incoming) >= lv_obj_get_height(viewport)) {
                printf("Transition y=%ld, active animations=%u\n",
                       (long)lv_obj_get_y(incoming), (unsigned)lv_anim_count_running());
                return 16;
            }
            result = save_preview(display, "preview-transition.bmp");
            if (result) return result;
        }
        finish_animation();
    }
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), titles[0])) return 8;
    rf_ui_set_keys(1U << 0);
    rf_ui_set_keys(0);
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), titles[4])) return 9;
    rf_ui_set_keys((1U << 0) | (1U << 4));
    rf_ui_set_keys(0);
    rf_ui_set_keys((1U << 1) | (1U << 2));
    rf_ui_set_keys(0);
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), titles[4])) return 10;
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0);
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0); // Rapid presses replace the transition.
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), titles[1])) return 17;
    rf_ui_set_keys(1U << 0); rf_ui_set_keys(0);
    rf_ui_set_keys(1U << 0); rf_ui_set_keys(0);
    finish_animation(); // Settings on the first-level menu.
    rf_ui_set_keys(1U << 1); rf_ui_set_keys(1U << 1); rf_ui_set_keys(0);
    Sleep(70); lv_timer_handler(); lv_obj_update_layout(screen);
    unsigned entering = 0;
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_t *row = lv_obj_get_child(viewport, i);
        if (!strcmp(lv_label_get_text(lv_obj_get_child(row, 1)), "亮度")) {
            if (lv_obj_get_x(row) <= 2 || lv_obj_get_x(row) >= 162 ||
                lv_obj_get_style_opa(row, 0) == LV_OPA_COVER) return 52;
            entering++;
        }
    }
    if (entering != 1) return 53;
    int entry_capture = save_preview(display, "preview-enter-transition.bmp");
    if (entry_capture) return entry_capture;
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "亮度")) return 20;
    int result = save_preview(display, "preview-settings.bmp");
    if (result) return result;
    const char *setting_titles[] = {"亮度", "休眠", "唤醒", "重置", "关于", "自动保存", "启动接收"};
    for (unsigned i = 0; i < 7; i++) {
        char expected[16], filename[40];
        snprintf(expected, sizeof(expected), "%u/7", i + 1);
        if (strcmp(lv_label_get_text(visible_menu_label(viewport)), setting_titles[i]) ||
            (i < 5 && strcmp(lv_label_get_text(counter), expected))) return 38;
        lv_obj_update_layout(screen);
        lv_obj_t *label = visible_menu_label(viewport);
        if (!fits_inside(label, viewport, 0) ||
            !fits_inside(lv_obj_get_child(lv_obj_get_parent(label), 0), viewport, 0)) return 39;
        snprintf(filename, sizeof(filename), "preview-settings-%u.bmp", i + 1);
        result = save_preview(display, filename);
        if (result) return result;
        if (i == 1) {
            rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
            uint8_t light, deep;
            rf_ui_get_sleep_minutes(&light, &deep);
            lv_obj_t *sleep_row = lv_obj_get_parent(visible_menu_label(viewport));
            if (light != 10 || deep != 20 ||
                strcmp(lv_label_get_text(lv_obj_get_child(sleep_row, 3)), "Light\nDeep") ||
                strcmp(lv_label_get_text(lv_obj_get_child(sleep_row, 4)), "10 min\n20 min")) return 84;
            rf_ui_set_keys(1U << 0); rf_ui_set_keys(0);
            rf_ui_get_sleep_minutes(&light, &deep);
            if (light != 11 || deep != 20) return 85;
            rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
            rf_ui_set_keys(1U << 4); rf_ui_set_keys(0);
            rf_ui_get_sleep_minutes(&light, &deep);
            if (light != 11 || deep != 19) return 86;
            rf_ui_set_sleep_minutes(120, 120);
            lv_obj_update_layout(screen);
            for (unsigned child = 3; child < 5; child++)
                if (lv_obj_has_flag(lv_obj_get_child(sleep_row, child), LV_OBJ_FLAG_HIDDEN) ||
                    !fits_inside(lv_obj_get_child(sleep_row, child), viewport, 0)) return 87;
            result = save_preview(display, "preview-sleep-deep.bmp"); if (result) return result;
            rf_ui_set_keys(1U << 0); rf_ui_set_keys(0);
            rf_ui_get_sleep_minutes(&light, &deep); if (deep != 120) return 88;
            rf_ui_set_sleep_minutes(0, 0);
            rf_ui_set_keys(1U << 4); rf_ui_set_keys(0);
            rf_ui_get_sleep_minutes(&light, &deep);
            if (deep || strcmp(lv_label_get_text(lv_obj_get_child(sleep_row, 4)), "OFF\nOFF")) return 89;
            lv_obj_update_layout(screen);
            if (!fits_inside(lv_obj_get_child(sleep_row, 4), viewport, 0)) return 90;
            rf_ui_set_sleep_minutes(10, 20);
            rf_ui_set_keys(1U << 1); rf_ui_set_keys(0); finish_animation();
            result = save_preview(display, "preview-sleep-light.bmp"); if (result) return result;
            rf_ui_set_keys(1U << 2); rf_ui_set_keys(0); finish_animation();
            if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "休眠")) return 91;
        } else if (i >= 5) {
            uint8_t before = rf_ui_get_receive_options();
            rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
            if (rf_ui_get_receive_options() != (before ^ (1U << (i-5))) ||
                strcmp(lv_label_get_text(counter), "ON  ENTER SWITCH")) return 92;
            snprintf(filename, sizeof(filename), "preview-settings-%u-on.bmp", i+1);
            result = save_preview(display, filename); if (result) return result;
            rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
            if (rf_ui_get_receive_options() != before) return 93;
        } else if (i > 1) {
            rf_ui_set_keys(1U << 3); rf_ui_set_keys(0);
    finish_animation(); // Menu entry only; no action yet.
            if (strcmp(lv_label_get_text(visible_menu_label(viewport)), setting_titles[i])) return 40;
        }
        rf_ui_set_keys(1U << 4); rf_ui_set_keys(0);
        finish_animation();
    }
    rf_ui_set_keys(1U << 0); rf_ui_set_keys(0);
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "启动接收")) return 41;
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0);
    finish_animation();
    rf_ui_set_keys(1U << 1); rf_ui_set_keys(0);
    finish_animation();
    if (rf_ui_get_brightness() != 70) return 21;
    rf_ui_set_keys(1U << 0); rf_ui_set_keys(1U << 0); rf_ui_set_keys(0);
    if (rf_ui_get_brightness() != 71) return 22;
    for (unsigned i = 0; i < 40; i++) { rf_ui_set_keys(1U << 0); rf_ui_set_keys(0); }
    if (rf_ui_get_brightness() != 100) return 23;
    lv_obj_update_layout(screen);
    if (!fits_inside(visible_menu_label(viewport), viewport, 0)) return 24;
    result = save_preview(display, "preview-brightness.bmp");
    if (result) return result;
    for (unsigned i = 0; i < 110; i++) { rf_ui_set_keys(1U << 4); rf_ui_set_keys(0); }
    if (rf_ui_get_brightness() != 1) return 25;
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0);
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "亮度")) return 26;
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0);
    Sleep(70); lv_timer_handler(); lv_obj_update_layout(screen);
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_t *row = lv_obj_get_child(viewport, i);
        if (!strcmp(lv_label_get_text(lv_obj_get_child(row, 1)), titles[4]) &&
            (lv_obj_get_x(row) >= 2 || lv_obj_get_x(row) <= -158)) return 54;
    }
    result = save_preview(display, "preview-return-transition.bmp");
    if (result) return result;
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), titles[4])) return 27;
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0);
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0);
    rf_ui_set_keys(1U << 0); rf_ui_set_keys(0);
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0);
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "开始调试")) return 42;
    unsigned visible_rows = 0;
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_t *row = lv_obj_get_child(viewport, i);
        if (!lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
            visible_rows++;
            if (lv_obj_get_x(row) != 2 || lv_obj_get_y(row) != 0 ||
                lv_obj_get_style_opa(row, 0) != LV_OPA_COVER) { printf("row x=%ld y=%ld opa=%u animations=%u\n", (long)lv_obj_get_x(row), (long)lv_obj_get_y(row), lv_obj_get_style_opa(row, 0), (unsigned)lv_anim_count_running()); return 55; }
        }
    }
    if (visible_rows != 1) return 56;
    lv_obj_update_layout(screen);
    if (!fits_inside(visible_menu_label(viewport), viewport, 0)) return 43;
    result = save_preview(display, "preview-debug-start.bmp");
    if (result) return result;
    lv_obj_send_event(lv_obj_get_parent(visible_menu_label(viewport)), LV_EVENT_CLICKED, NULL);
    if (!rf_ui_take_debug_request() || rf_ui_take_debug_request()) return 44;
    Sleep(60); lv_timer_handler(); lv_obj_update_layout(screen);
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_t *row = lv_obj_get_child(viewport, i);
        if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) ||
            lv_obj_get_style_opa(row, 0) == LV_OPA_TRANSP ||
            lv_obj_get_style_opa(row, 0) == LV_OPA_COVER) return 57;
    }
    result = save_preview(display, "preview-debug-transition.bmp");
    if (result) return result;
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "启动中")) return 45;
    rf_ui_set_debug_state(RF_DEBUG_READY);
    finish_animation();
    if (strcmp(lv_label_get_text(counter), "192.168.4.1")) return 46;
    result = save_preview(display, "preview-debug-ready.bmp");
    if (result) return result;
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0);
    finish_animation();
    if (!rf_ui_take_debug_request() || rf_ui_take_debug_request()) return 47;
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "关闭中")) return 50;
    rf_ui_set_debug_state(RF_DEBUG_OFF);
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "开始调试")) return 51;
    rf_ui_set_debug_state(RF_DEBUG_ERROR);
    finish_animation();
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0);
    finish_animation();
    if (!rf_ui_take_debug_request()) return 48;
    rf_ui_set_debug_state(RF_DEBUG_READY);
    finish_animation();
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0);
    finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), titles[3])) return 49;
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0);
    finish_animation();
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0);
    finish_animation();
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0);
    finish_animation();
    test_clock = lv_tick_get();
    lv_tick_set_cb(test_tick); // Deterministic clock for the real LVGL repeat timer.
    rf_ui_set_keys(1U << 0);
    if (rf_ui_get_brightness() != 2) return 28;
    advance_test_time(390);
    if (rf_ui_get_brightness() != 2) return 29;
    advance_test_time(10);
    if (rf_ui_get_brightness() != 3) return 30;
    unsigned start = rf_ui_get_brightness();
    advance_test_time(600); // Hold time 400 -> 1000 ms, slow repeats.
    unsigned slow = rf_ui_get_brightness() - start;
    advance_test_time(500);
    start = rf_ui_get_brightness();
    advance_test_time(600); // Hold time 1500 -> 2100 ms: speed remains fixed.
    unsigned medium = rf_ui_get_brightness() - start;
    advance_test_time(900);
    start = rf_ui_get_brightness();
    advance_test_time(600); // Hold time 3000 -> 3600 ms: speed remains fixed.
    unsigned fast = rf_ui_get_brightness() - start;
    if (slow != 5 || medium != 5 || fast != 5) return 31;
    start = rf_ui_get_brightness();
    rf_ui_set_keys((1U << 0) | (1U << 4));
    advance_test_time(200);
    if (rf_ui_get_brightness() != start) return 32;
    rf_ui_set_keys(0);
    rf_ui_set_keys(1U << 4);
    if (rf_ui_get_brightness() != start - 1) return 33;
    advance_test_time(390);
    if (rf_ui_get_brightness() != start - 1) return 34;
    advance_test_time(10);
    if (rf_ui_get_brightness() != start - 2) return 35;
    rf_ui_set_keys(0);
    start = rf_ui_get_brightness();
    advance_test_time(1000);
    if (rf_ui_get_brightness() != start) return 36;
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0);
    finish_animation();
    advance_test_time(1000);
    if (rf_ui_get_brightness() != start) return 37;
    lv_tick_set_cb(preview_tick);
    lv_obj_clean(screen); rf_ui_create(); rf_ui_set_battery_mv(3850);
    viewport = lv_obj_get_child(screen, 1); counter = lv_obj_get_child(screen, 2);
    rf_command_t request;
    rf_status_t radio = {.revision = 1, .frequency_hz = 433920000, .rssi_dbm = -67};
    rf_ui_update_radio(&radio);
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "自动")) return 60;
    // Up wraps to the new 318 MHz option; verify the action carries that frequency.
    rf_ui_set_keys(1U << 0); rf_ui_set_keys(0); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "318MHz")) return 80;
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_RECEIVE || request.frequency_hz != 318000000) return 81;
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0); finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_STOP) return 82;
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "自动")) return 83;
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0); finish_animation();
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_RECEIVE || request.frequency_hz != 433920000) return 61;
    radio.state = RF_LISTENING; radio.revision++; rf_ui_update_radio(&radio); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "接收中")) return 62;
    radio.state = RF_CAPTURED; radio.bits = 0; radio.raw = true; radio.pulse_count = 82; strcpy(radio.code, "2D789C0555");
    radio.revision++; rf_ui_update_radio(&radio); finish_animation();
    result = save_preview(display, "preview-receive.bmp"); if (result) return result;
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "2D789C0555")) return 63;
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_SAVE) return 64;
    radio.state = RF_SAVED; radio.saved_id = 7; radio.library_count = 1; radio.library[0].id = 7;
    radio.library[0].frequency_hz = 433920000;
    strcpy(radio.library[0].code,"12AB01"); radio.revision++; rf_ui_update_radio(&radio); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "2D789C0555")) return 65;
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0); finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_STOP) return 66;
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0); finish_animation(); rf_ui_take_radio_action(&request);
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0); finish_animation();
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_LIBRARY) return 67;
    lv_obj_t *library_box = lv_obj_get_parent(visible_menu_label(viewport));
    if (strcmp(lv_label_get_text(lv_obj_get_child(library_box, 3)), "433.92_12AB01")) return 68;
    // Five entries exercise three rows, a partial final page and wraparound.
    radio.library_count = 5;
    for (unsigned i = 1; i < 5; i++) {
        radio.library[i].id = 7 + i;
        radio.library[i].frequency_hz = i % 2 ? 315000000 : 318000000;
        snprintf(radio.library[i].code, sizeof(radio.library[i].code), "%06X", 0x12AB01 + i);
    }
    radio.revision++; rf_ui_update_radio(&radio); finish_animation();
    library_box = lv_obj_get_parent(visible_menu_label(viewport));
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *entry = lv_obj_get_child(library_box, 3 + i);
        if (lv_obj_has_flag(entry, LV_OBJ_FLAG_HIDDEN) || !fits_inside(entry, viewport, 0)) return 84;
    }
    result = save_preview(display, "preview-library.bmp"); if (result) return result;
    // Check the actual highlight animation and retarget it before completion.
    lv_obj_t *highlight = lv_obj_get_child(library_box, 2);
    test_clock = lv_tick_get(); lv_tick_set_cb(test_tick);
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0);
    advance_test_time(60); lv_obj_update_layout(screen);
    if (lv_obj_get_y(highlight) <= 1 || lv_obj_get_y(highlight) >= 16) return 88;
    result = save_preview(display, "preview-library-highlight.bmp"); if (result) return result;
    int32_t moving_y = lv_obj_get_y(highlight);
    rf_ui_set_keys(1U << 0); rf_ui_set_keys(0); lv_obj_update_layout(screen);
    if (lv_obj_get_y(highlight) != moving_y) return 89;
    advance_test_time(200); lv_obj_update_layout(screen);
    if (lv_obj_get_y(highlight) != 1 || !fits_inside(highlight, viewport, 0)) return 90;
    lv_tick_set_cb(preview_tick);
    // Clicking an unselected row sends that row exactly once.
    lv_obj_send_event(lv_obj_get_child(library_box, 4), LV_EVENT_CLICKED, NULL);
    finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_SEND || request.id != 8 ||
        rf_ui_take_radio_action(&request)) return 91;
    if (lv_obj_get_parent(visible_menu_label(viewport)) != library_box ||
        strcmp(lv_label_get_text(counter), "1/2  MHz")) return 94;
    // Navigation remains available while the send is pending or in progress.
    rf_ui_set_keys(1U << 1); rf_ui_set_keys(0); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "发射信号") ||
        rf_ui_take_radio_action(&request)) return 99;
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0); finish_animation();
    radio.state = RF_SENDING; radio.revision++; rf_ui_update_radio(&radio);
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0);
    if (rf_ui_take_radio_action(&request)) return 95;
    library_box = lv_obj_get_parent(visible_menu_label(viewport));
    lv_area_t busy_area;
    lv_obj_get_coords(lv_obj_get_child(library_box, 4), &busy_area);
    rf_ui_signal_context_at(busy_area.x1 + 2, busy_area.y1 + 2); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "发射信号") ||
        !strstr(lv_label_get_text(counter), "12AB02") || rf_ui_take_radio_action(&request)) return 100;
    radio.state = RF_SENT; radio.revision++; rf_ui_update_radio(&radio); finish_animation();
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0); finish_animation();
    rf_ui_set_keys(1U << 0); rf_ui_set_keys(0); finish_animation();
    for (unsigned i = 0; i < 3; i++) { rf_ui_set_keys(1U << 4); rf_ui_set_keys(0); finish_animation(); }
    library_box = lv_obj_get_parent(visible_menu_label(viewport));
    if (strcmp(lv_label_get_text(counter), "2/2  MHz") ||
        strcmp(lv_label_get_text(lv_obj_get_child(library_box, 3)), "315.00_12AB04") ||
        !lv_obj_has_flag(lv_obj_get_child(library_box, 5), LV_OBJ_FLAG_HIDDEN)) return 85;
    result = save_preview(display, "preview-library-page2.bmp"); if (result) return result;
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_SEND || request.id != 10) return 86;
    if (strcmp(lv_label_get_text(counter), "2/2  MHz")) return 96;
    radio.state = RF_SENT; radio.revision++; rf_ui_update_radio(&radio); finish_animation();
    for (unsigned i = 0; i < 2; i++) { rf_ui_set_keys(1U << 4); rf_ui_set_keys(0); finish_animation(); }
    if (strcmp(lv_label_get_text(counter), "1/2  MHz")) return 87;
    // Right-click another row opens actions without sending it.
    library_box = lv_obj_get_parent(visible_menu_label(viewport));
    lv_area_t context_area;
    lv_obj_get_coords(lv_obj_get_child(library_box, 5), &context_area);
    rf_ui_signal_context_at(context_area.x1 + 2, context_area.y1 + 2); finish_animation();
    if (rf_ui_take_radio_action(&request) ||
        strcmp(lv_label_get_text(visible_menu_label(viewport)), "发射信号") ||
        !strstr(lv_label_get_text(counter), "12AB03")) return 92;
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0);
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_SEND || request.id != 9 ||
        strcmp(lv_label_get_text(visible_menu_label(viewport)), "发射信号")) return 97;
    radio.state = RF_FAILURE; radio.revision++; rf_ui_update_radio(&radio); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "发射信号")) return 98;
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0); finish_animation();
    for (unsigned i = 0; i < 2; i++) { rf_ui_set_keys(1U << 0); rf_ui_set_keys(0); finish_animation(); }
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    if (strcmp(lv_label_get_text(counter), "1/2  MHz")) return 69;
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_SEND || request.id != 7) return 70;
    radio.state = RF_SENT; radio.revision++; rf_ui_update_radio(&radio); finish_animation();
    if (strcmp(lv_label_get_text(counter), "1/2  MHz")) return 71;
    rf_ui_set_keys(1U << 1); rf_ui_set_keys(0); finish_animation();
    if (rf_ui_take_radio_action(&request) || strcmp(lv_label_get_text(visible_menu_label(viewport)), "发射信号")) return 93;
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0); finish_animation();
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "确认删除")) return 72;
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_DELETE || request.id != 7) return 73;
    radio.library_count = 0; radio.state = RF_IDLE; radio.revision++; rf_ui_update_radio(&radio); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "暂无信号")) return 74;
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0); finish_animation(); rf_ui_take_radio_action(&request);
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0); finish_animation();
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    rf_ui_set_keys(1U << 3); rf_ui_set_keys(0); finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_MONITOR) return 75;
    radio.state = RF_MONITORING; radio.occupancy = 37; radio.revision++; rf_ui_update_radio(&radio); finish_animation();
    rf_ui_set_keys(1U << 4); rf_ui_set_keys(0); finish_animation();
    if (strcmp(lv_label_get_text(visible_menu_label(viewport)), "占用37%")) return 76;
    if (!strstr(lv_label_get_text(counter), "433.92MHz")) return 80;
    result = save_preview(display, "preview-monitor.bmp"); if (result) return result;
    lv_obj_update_layout(screen);
    if (!fits_inside(visible_menu_label(viewport), viewport, 0) || !fits_inside(counter, screen, 0)) return 77;
    rf_ui_set_keys(1U << 2); rf_ui_set_keys(0); finish_animation();
    if (!rf_ui_take_radio_action(&request) || request.type != RF_CMD_STOP) return 78;
    return 0;
}

int main(int argc, char **argv)
{
    bool test = argc > 1 && strcmp(argv[1], "--smoke-test") == 0;
    lv_init();
    lv_display_t *display = lv_windows_create_display(
        L"RF Manager - 160x80 - Up / Down", 160, 80, 400, true, true);
    if (!display) { fputs("Cannot create preview window.\n", stderr); return 1; }
    HWND window = lv_windows_get_display_window_handle(display);
    if (test) ShowWindow(window, SW_HIDE);
    // The native driver installs its framebuffer on a timer. An earlier refresh
    // pauses LVGL's refresh timer, so wait for the buffer before creating the UI.
    bool framebuffer_ready = false;
    for (unsigned i = 0; i < 200; i++) {
        lv_lock();
        if (!IsWindow(window)) { lv_unlock(); return 1; }
        lv_timer_handler();
        framebuffer_ready = lv_display_get_buf_active(display) != NULL;
        lv_unlock();
        if (framebuffer_ready) break;
        Sleep(5);
    }
    if (!framebuffer_ready) { fputs("Windows framebuffer timeout.\n", stderr); return 1; }
    lv_lock();
    rf_ui_create();
    rf_ui_set_battery_mv(3850); // Preview-only example; firmware uses measured data.
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display); // Paint the first complete menu before the main loop.
    // Native LVGL key events also support UI groups added in rf_ui_create().
    lv_indev_t *keypad = lv_windows_acquire_keypad_indev(display);
    lv_indev_t *pointer = lv_windows_acquire_pointer_indev(display);
    lv_unlock();
    if (!keypad || !pointer) { fputs("Cannot create input devices.\n", stderr); return 1; }
    if (test) {
        for (unsigned i = 0; i < 40; i++) {
            lv_lock();
            lv_timer_handler();
            lv_unlock();
            Sleep(5);
        }
        lv_lock();
        int result = smoke_test(display);
        lv_unlock();
        printf("Preview smoke test: %s (%d)\n", result ? "FAIL" : "PASS", result);
        return result;
    }
    lv_lock();
    lv_timer_t *keyboard_timer = lv_timer_create(scan_preview_keys, 5, window);
    lv_timer_t *debug_timer = lv_timer_create(simulate_debug, 50, NULL);
    lv_timer_t *radio_timer = lv_timer_create(simulate_radio, 50, NULL);
    lv_unlock();
    if (!keyboard_timer || !debug_timer || !radio_timer) { fputs("Cannot create preview timers.\n", stderr); return 1; }
    for (;;) {
        lv_lock();
        if (!IsWindow(window)) { lv_unlock(); break; }
        lv_timer_handler();
        lv_unlock();
        Sleep(5);
    }
    return 0;
}
