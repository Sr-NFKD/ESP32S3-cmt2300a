#include "rf_ui.h"
#include "lvgl.h"
#include <string.h>

static const char *signal_label(const rf_library_item_t *item)
{
    const char *code = item->code;
    if (item->raw) return code;
    size_t length = strlen(code);
    return code + (length > 6 ? length - 6 : 0);
}

LV_FONT_DECLARE(rf_font_menu_32);
LV_FONT_DECLARE(rf_font_debug_22);
static const char *const menu[] = {
    "接收", "发射", "干扰", "调试", "设置"
};
static const char *const icons[] = {
    LV_SYMBOL_DOWNLOAD, LV_SYMBOL_UPLOAD, LV_SYMBOL_WARNING, LV_SYMBOL_WIFI, LV_SYMBOL_SETTINGS
};
static const uint32_t menu_colors[] = {
    0x63DAB2, 0xFFAE63, 0xF17385, 0xB096FA, 0xBEC9D8
};
static const char *const settings[] = {"亮度", "休眠", "唤醒", "重置", "关于", "自动保存", "启动接收"};
static const char *const settings_icons[] = {
    LV_SYMBOL_EYE_OPEN, LV_SYMBOL_PAUSE, LV_SYMBOL_CHARGE, LV_SYMBOL_REFRESH, LV_SYMBOL_LIST,
    LV_SYMBOL_SAVE, LV_SYMBOL_POWER
};
static const uint32_t settings_colors[] = {
    0xF4C75E, 0xAC9DEE, 0x75DAAD, 0xF28B87, 0x81BDE7, 0x63DAB2, 0xFFAE63
};
#define MENU_COUNT (sizeof(menu) / sizeof(menu[0]))
#define SETTINGS_COUNT (sizeof(settings) / sizeof(settings[0]))
#define ROW_HEIGHT 48
#define TRANSITION_MS 320
#define ROW_X 2
#define VIEW_WIDTH 160
#define SIGNALS_PER_PAGE 3
typedef struct {
    lv_obj_t *box, *icon, *label, *highlight, *signals[SIGNALS_PER_PAGE];
    int32_t highlight_target;
} menu_row_t;
static menu_row_t rows[2];
static unsigned active_row;
static lv_obj_t *page_label;
static lv_obj_t *battery_label;
static lv_obj_t *battery_raw_label;
static unsigned page;
static unsigned settings_page;
static uint8_t receive_options;
static uint32_t previous_keys;
typedef enum { VIEW_MENU, VIEW_SETTINGS, VIEW_BRIGHTNESS, VIEW_SLEEP, VIEW_DEBUG, VIEW_RECEIVE, VIEW_CAPTURE, VIEW_LIBRARY, VIEW_SIGNAL, VIEW_DELETE, VIEW_MONITOR, VIEW_MONITOR_RUNNING } ui_view_t;
static ui_view_t view;
static uint8_t brightness;
static uint8_t sleep_minutes[2];
static unsigned sleep_field;
static uint32_t repeat_key, hold_started, last_repeat;
static rf_debug_state_t debug_state;
static bool debug_request;
static rf_status_t radio_status;
static rf_command_t radio_action;
static unsigned frequency_page, library_page, signal_action;
static uint32_t selected_signal_id;
static bool radio_details, radio_waiting;
static const uint32_t frequencies[] = {0, 433920000, 315000000, 318000000};
#define FREQUENCY_COUNT (sizeof(frequencies) / sizeof(frequencies[0]))

static void request_radio(rf_command_type_t type)
{
    radio_action = (rf_command_t){.type = type, .frequency_hz = frequencies[frequency_page],
        .id = library_page < radio_status.library_count ? radio_status.library[library_page].id : 0};
    if (type == RF_CMD_SEND || type == RF_CMD_DELETE) radio_action.id = selected_signal_id;
    radio_waiting = type == RF_CMD_SAVE || type == RF_CMD_SEND || type == RF_CMD_DELETE;
}

bool rf_ui_take_radio_action(rf_command_t *command)
{
    if (!command || radio_action.type == RF_CMD_NONE) return false;
    *command = radio_action; radio_action.type = RF_CMD_NONE; return true;
}

static bool is_radio_view(void) { return view >= VIEW_RECEIVE; }
static void slide_y(void *object, int32_t y);
static void animate(lv_obj_t *object, lv_anim_exec_xcb_t callback,
                    int32_t from, int32_t to, uint32_t duration, bool finish);

static void set_row(menu_row_t *row)
{
    lv_color_t accent = lv_color_hex(is_radio_view() ? menu_colors[view == VIEW_RECEIVE || view == VIEW_CAPTURE ? 0 : view == VIEW_MONITOR || view == VIEW_MONITOR_RUNNING ? 2 : 1] : view == VIEW_MENU ? menu_colors[page] :
        view == VIEW_DEBUG ? menu_colors[3] :
        view == VIEW_SETTINGS ? settings_colors[settings_page] : settings_colors[0]);
    lv_obj_set_style_text_color(row->icon, accent, 0);
    lv_obj_set_style_text_color(row->label, lv_color_hex(0xF4F4F1), 0);
    lv_obj_set_style_bg_color(row->box, lv_color_mix(accent, lv_color_hex(0x17191E), 24), 0);
    lv_obj_set_style_bg_grad_color(row->box, lv_color_hex(0x15161B), 0);
    lv_obj_set_style_border_color(row->box, lv_color_mix(accent, lv_color_hex(0x272A32), 80), 0);
    lv_obj_set_style_text_color(page_label, lv_color_mix(accent, lv_color_hex(0x9BA1AB), 110), 0);
    lv_obj_set_style_text_font(row->label, &rf_font_menu_32, 0);
    lv_obj_remove_flag(row->box, LV_OBJ_FLAG_CLICKABLE);
    bool show_library = view == VIEW_LIBRARY && radio_status.library_count;
    bool show_sleep = view == VIEW_SLEEP;
    if (show_library || show_sleep) {
        lv_obj_add_flag(row->icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row->label, LV_OBJ_FLAG_HIDDEN);
        int32_t target = show_sleep ? 3 + sleep_field * 23 : 1 + (library_page % SIGNALS_PER_PAGE) * 15;
        if (!show_sleep) {
            lv_obj_set_x(row->highlight, 3);
            lv_obj_set_size(row->highlight, 148, 15);
        }
        bool moving = !lv_obj_has_flag(row->highlight, LV_OBJ_FLAG_HIDDEN) &&
                      !lv_obj_has_flag(row->box, LV_OBJ_FLAG_HIDDEN);
        if (!moving || row->highlight_target != target) {
            lv_anim_delete(row->highlight, slide_y);
            if (moving) animate(row->highlight, slide_y, lv_obj_get_y(row->highlight), target, 180, false);
            else lv_obj_set_y(row->highlight, target);
            row->highlight_target = target;
        }
        lv_obj_remove_flag(row->highlight, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(row->icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(row->label, LV_OBJ_FLAG_HIDDEN);
        lv_anim_delete(row->highlight, slide_y);
        lv_obj_add_flag(row->highlight, LV_OBJ_FLAG_HIDDEN);
    }
    for (unsigned i = 0; i < SIGNALS_PER_PAGE; i++) {
        lv_obj_add_flag(row->signals[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(row->signals[i], 148, 15);
        lv_obj_set_pos(row->signals[i], 3, 1 + i * 15);
        lv_obj_set_style_text_align(row->signals[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_line_space(row->signals[i], 0, 0);
    }
    if (is_radio_view()) {
        lv_obj_set_style_text_font(row->label, &rf_font_debug_22, 0);
        const bool receive_view = view == VIEW_RECEIVE || view == VIEW_CAPTURE;
        const bool monitor_view = view == VIEW_MONITOR || view == VIEW_MONITOR_RUNNING;
        lv_label_set_text(row->icon, receive_view ? LV_SYMBOL_DOWNLOAD : monitor_view ? LV_SYMBOL_WARNING : LV_SYMBOL_UPLOAD);
        lv_label_set_text(page_label, "ENTER> LEFT<");
        if (view == VIEW_RECEIVE || view == VIEW_MONITOR) {
            if (!frequency_page) lv_label_set_text(row->label, "自动");
            else lv_label_set_text_fmt(row->label, "%luMHz", (unsigned long)(frequencies[frequency_page]/1000000));
            lv_label_set_text(page_label, view == VIEW_RECEIVE ? "UP/DOWN  ENTER RX" : "UP/DOWN  ENTER RSSI");
        } else if (view == VIEW_CAPTURE) {
            const char *title = radio_waiting ? "等待中" : radio_status.state == RF_CAPTURED ? "保存信号" :
                radio_status.state == RF_SAVED ? "已保存" : radio_status.state == RF_FAILURE ? "操作失败" : radio_status.state == RF_IDLE ? "等待中" : "接收中";
            lv_label_set_text(row->label, title);
            if ((radio_details || strncmp(radio_status.code, "RAW", 3)) && radio_status.code[0] &&
                (radio_status.state == RF_CAPTURED || radio_status.state == RF_SAVED)) {
                lv_label_set_text(row->label, radio_status.code);
                lv_obj_set_style_text_font(row->label, &lv_font_montserrat_14, 0);
            }
            if (radio_status.state == RF_CAPTURED || radio_status.state == RF_SAVED)
                lv_label_set_text_fmt(page_label, "%uP  %s", radio_status.pulse_count,
                    radio_status.state == RF_SAVED ? "SAVED" : "ENTER SAVE");
            else if (radio_status.state == RF_FAILURE) lv_label_set_text(page_label, radio_status.error);
            else lv_label_set_text_fmt(page_label, "%lu.%02luMHz %ddBm",
                (unsigned long)(radio_status.frequency_hz/1000000),
                (unsigned long)((radio_status.frequency_hz/10000)%100), radio_status.rssi_dbm);
        } else if (view == VIEW_LIBRARY) {
            if (!radio_status.library_count) lv_label_set_text(row->label, "暂无信号");
            else {
                unsigned first = library_page / SIGNALS_PER_PAGE * SIGNALS_PER_PAGE;
                for (unsigned i = 0; i < SIGNALS_PER_PAGE && first + i < radio_status.library_count; i++) {
                    const rf_library_item_t *item = &radio_status.library[first + i];
                    lv_obj_t *label = row->signals[i];
                    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
                    lv_label_set_text_fmt(label, "%lu.%02lu_%s", (unsigned long)(item->frequency_hz / 1000000),
                        (unsigned long)(item->frequency_hz / 10000 % 100), signal_label(item));
                    bool selected = first + i == library_page;
                    lv_obj_set_style_text_color(label, selected ? accent : lv_color_hex(0xBAC0CA), 0);
                    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
                }
                lv_label_set_text_fmt(page_label, "%u/%u  MHz", library_page / SIGNALS_PER_PAGE + 1,
                    (radio_status.library_count + SIGNALS_PER_PAGE - 1) / SIGNALS_PER_PAGE);
            }
        } else if (view == VIEW_SIGNAL || view == VIEW_DELETE) {
            lv_label_set_text(row->label, view == VIEW_DELETE ? "确认删除" : signal_action ? "删除信号" : "发射信号");
            if (library_page < radio_status.library_count)
                lv_label_set_text_fmt(page_label, "%lu.%02lu_%s",
                    (unsigned long)(radio_status.library[library_page].frequency_hz / 1000000),
                    (unsigned long)(radio_status.library[library_page].frequency_hz / 10000 % 100), signal_label(&radio_status.library[library_page]));
        } else {
            if (radio_status.state == RF_FAILURE) { lv_label_set_text(row->label, "操作失败"); lv_label_set_text(page_label, radio_status.error); }
            else if (radio_status.state == RF_IDLE) lv_label_set_text(row->label, "等待中");
            else {
                if (radio_details) lv_label_set_text_fmt(row->label, "占用%u%%", radio_status.occupancy);
                else lv_label_set_text_fmt(row->label, "%ddBm", radio_status.rssi_dbm);
                lv_label_set_text_fmt(page_label, "%lu.%02luMHz ENTER STOP",
                    (unsigned long)(radio_status.frequency_hz/1000000),
                    (unsigned long)((radio_status.frequency_hz/10000)%100));
            }
        }
        lv_obj_add_flag(row->box, LV_OBJ_FLAG_CLICKABLE);
    } else if (view == VIEW_SLEEP) {
        char light[12], deep[12];
        if (sleep_minutes[0]) lv_snprintf(light, sizeof(light), "%u min", sleep_minutes[0]);
        else lv_snprintf(light, sizeof(light), "OFF");
        if (sleep_minutes[1]) lv_snprintf(deep, sizeof(deep), "%u min", sleep_minutes[1]);
        else lv_snprintf(deep, sizeof(deep), "OFF");
        lv_point_t text_size;
        lv_text_get_size(&text_size, sleep_field ? deep : light, &lv_font_montserrat_14,
                         0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_x(row->highlight, 148 - text_size.x - 2);
        lv_obj_set_size(row->highlight, text_size.x + 4, text_size.y + 2);
        lv_label_set_text(row->signals[0], "Light\nDeep");
        lv_label_set_text_fmt(row->signals[1], "%s\n%s", light, deep);
        for (unsigned i = 0; i < 2; i++) {
            lv_obj_remove_flag(row->signals[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(row->signals[i], i ? 70 : 60, 42);
            lv_obj_set_pos(row->signals[i], i ? 78 : 8, 4);
            lv_label_set_long_mode(row->signals[i], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_line_space(row->signals[i], 7, 0);
            lv_obj_set_style_text_color(row->signals[i], accent, 0);
        }
        lv_obj_set_style_text_align(row->signals[1], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(page_label, "UP+ DOWN- ENTER> LEFT<");
    } else if (view == VIEW_DEBUG) {
        lv_obj_set_style_text_font(row->label, &rf_font_debug_22, 0);
        lv_label_set_text(row->icon, LV_SYMBOL_WIFI);
        lv_label_set_text(row->label, debug_state == RF_DEBUG_OFF ? "开始调试" :
            debug_state == RF_DEBUG_STARTING ? "启动中" : debug_state == RF_DEBUG_STOPPING ? "关闭中" : debug_state == RF_DEBUG_READY ? "已开启" : "重试");
        lv_label_set_text(page_label, debug_state == RF_DEBUG_READY ? "192.168.4.1" : "ENTER>  LEFT<");
        if (debug_state == RF_DEBUG_OFF || debug_state == RF_DEBUG_READY || debug_state == RF_DEBUG_ERROR)
            lv_obj_add_flag(row->box, LV_OBJ_FLAG_CLICKABLE);
    } else if (view == VIEW_MENU) {
        lv_label_set_text(row->label, menu[page]);
        lv_label_set_text(row->icon, icons[page]);
        lv_label_set_text_fmt(page_label, "%u/%u", page + 1, (unsigned)MENU_COUNT);
    } else {
        if (view == VIEW_SETTINGS) {
            lv_label_set_text(row->icon, settings_icons[settings_page]);
            lv_label_set_text(row->label, settings[settings_page]);
            lv_label_set_text_fmt(page_label, "%u/%u", settings_page + 1, (unsigned)SETTINGS_COUNT);
            if (settings_page >= 5) {
                lv_obj_set_style_text_font(row->label, &rf_font_debug_22, 0);
                lv_label_set_text_fmt(page_label, "%s  ENTER SWITCH",
                    receive_options & (1U << (settings_page-5)) ? "ON" : "OFF");
            }
        } else {
            lv_label_set_text(row->icon, LV_SYMBOL_EYE_OPEN);
            lv_obj_set_style_text_font(row->label, &lv_font_montserrat_36, 0);
            lv_label_set_text_fmt(row->label, "%u%%", brightness);
            lv_label_set_text(page_label, "UP+  DOWN-  LEFT<");
        }
    }
}

static void slide_y(void *object, int32_t y) { lv_obj_set_y(object, y); }
static void slide_x(void *object, int32_t x) { lv_obj_set_x(object, x); }
static void fade(void *object, int32_t opacity) { lv_obj_set_style_opa(object, opacity, 0); }

static void transition_done(lv_anim_t *animation)
{
    (void)animation;
    lv_obj_add_flag(rows[1 - active_row].box, LV_OBJ_FLAG_HIDDEN);
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_set_pos(rows[i].box, ROW_X, 0);
        lv_obj_set_style_opa(rows[i].box, LV_OPA_COVER, 0);
    }
}

static void settle_transition(void)
{
    for (unsigned i = 0; i < 2; i++) {
        lv_anim_delete(rows[i].box, slide_x);
        lv_anim_delete(rows[i].box, slide_y);
        lv_anim_delete(rows[i].box, fade);
    }
    transition_done(NULL);
}

static void animate(lv_obj_t *object, lv_anim_exec_xcb_t callback,
                    int32_t from, int32_t to, uint32_t duration, bool finish)
{
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, callback);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_path_cb(&animation, callback == fade ? lv_anim_path_ease_in_out : lv_anim_path_ease_out);
    if (finish) lv_anim_set_completed_cb(&animation, transition_done);
    lv_anim_start(&animation);
}

typedef enum { TRANSITION_VERTICAL, TRANSITION_HORIZONTAL, TRANSITION_FADE } transition_t;

static void change_view(transition_t effect, int direction)
{
    // Rapid input settles the last target, then transitions to the newest target.
    settle_transition();
    unsigned outgoing = active_row;
    active_row = 1 - active_row;
    set_row(&rows[active_row]);
    lv_obj_remove_flag(rows[active_row].box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(rows[outgoing].box, LV_OBJ_FLAG_CLICKABLE);
    uint32_t duration = effect == TRANSITION_FADE ? 280 : TRANSITION_MS;
    animate(rows[outgoing].box, fade, LV_OPA_COVER, LV_OPA_TRANSP, duration, false);
    animate(rows[active_row].box, fade, LV_OPA_TRANSP, LV_OPA_COVER, duration,
            effect == TRANSITION_FADE);
    if (effect != TRANSITION_FADE) {
        lv_anim_exec_xcb_t callback = effect == TRANSITION_HORIZONTAL ? slide_x : slide_y;
        int32_t origin = effect == TRANSITION_HORIZONTAL ? ROW_X : 0;
        int32_t distance = effect == TRANSITION_HORIZONTAL ? VIEW_WIDTH : ROW_HEIGHT;
        animate(rows[outgoing].box, callback, origin, origin - direction * distance, duration, false);
        animate(rows[active_row].box, callback, origin + direction * distance, origin, duration, true);
    }
    lv_anim_delete(page_label, fade);
    animate(page_label, fade, LV_OPA_TRANSP, LV_OPA_COVER, duration, false);
}

static void request_debug_start(void)
{
    if (view != VIEW_DEBUG || (debug_state != RF_DEBUG_OFF && debug_state != RF_DEBUG_READY && debug_state != RF_DEBUG_ERROR)) return;
    debug_request = true;
    debug_state = debug_state == RF_DEBUG_READY ? RF_DEBUG_STOPPING : RF_DEBUG_STARTING;
    change_view(TRANSITION_FADE, 0);
}

static bool radio_key(uint32_t rising);

static void signal_clicked(lv_event_t *event)
{
    if (view != VIEW_LIBRARY || radio_waiting) return;
    lv_obj_t *target = lv_event_get_target(event);
    for (unsigned i = 0; i < SIGNALS_PER_PAGE; i++) {
        if (target != rows[active_row].signals[i]) continue;
        unsigned index = library_page / SIGNALS_PER_PAGE * SIGNALS_PER_PAGE + i;
        if (index >= radio_status.library_count) return;
        library_page = index;
        radio_key(1U << 3);
        return;
    }
}

void rf_ui_signal_context_at(int32_t x, int32_t y)
{
    if (view != VIEW_LIBRARY) return;
    for (unsigned i = 0; i < SIGNALS_PER_PAGE; i++) {
        lv_obj_t *label = rows[active_row].signals[i];
        if (lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_area_t area;
        lv_obj_get_coords(label, &area);
        if (x < area.x1 || x > area.x2 || y < area.y1 || y > area.y2) continue;
        library_page = library_page / SIGNALS_PER_PAGE * SIGNALS_PER_PAGE + i;
        radio_key(1U << 1);
        return;
    }
}

static void debug_button_clicked(lv_event_t *event)
{
    (void)event;
    if (view == VIEW_LIBRARY) return; // Only a signal row can send; blank space does nothing.
    if (is_radio_view()) radio_key(1U << 3);
    else request_debug_start();
}

bool rf_ui_take_debug_request(void)
{
    bool requested = debug_request;
    debug_request = false;
    return requested;
}

void rf_ui_set_debug_state(rf_debug_state_t state)
{
    if (state == debug_state) return;
    debug_state = state;
    if (view == VIEW_DEBUG) change_view(TRANSITION_FADE, 0);
}

void rf_ui_update_radio(const rf_status_t *status)
{
    if (!status || status->revision == radio_status.revision) return;
    rf_state_t before = radio_status.state;
    radio_status = *status;
    if (library_page >= radio_status.library_count) library_page = 0;
    if (view == VIEW_SIGNAL || view == VIEW_DELETE) {
        bool found = false;
        for (unsigned i = 0; i < radio_status.library_count; i++) if (radio_status.library[i].id == selected_signal_id) {
            library_page = i; found = true; break;
        }
        if (!found) { view = VIEW_LIBRARY; radio_waiting = false; }
    }
    if ((radio_status.state != before && radio_status.state != RF_SENDING) || radio_status.state == RF_SENT || radio_status.state == RF_SAVED ||
        radio_status.state == RF_FAILURE || radio_status.state == RF_IDLE) radio_waiting = false;
    if (is_radio_view()) {
        if (radio_status.state != before && view != VIEW_LIBRARY && view != VIEW_SIGNAL) change_view(TRANSITION_FADE, 0);
        else set_row(&rows[active_row]);
    }
}

void rf_ui_radio_error(const char *message)
{
    radio_status.state = RF_FAILURE; radio_waiting = false;
    lv_snprintf(radio_status.error, sizeof(radio_status.error), "%s", message ? message : "RF error");
    if (is_radio_view() && view != VIEW_LIBRARY && view != VIEW_SIGNAL) change_view(TRANSITION_FADE, 0);
}

bool rf_ui_remote_receive(uint32_t frequency_hz, bool start)
{
    unsigned index = 0;
    while (index < FREQUENCY_COUNT && frequencies[index] != frequency_hz) index++;
    if (index == FREQUENCY_COUNT) return false;
    frequency_page = index;
    page = 0; repeat_key = 0;
    radio_details = false;
    request_radio(start ? RF_CMD_RECEIVE : RF_CMD_STOP);
    view = start ? VIEW_CAPTURE : VIEW_RECEIVE;
    change_view(TRANSITION_HORIZONTAL, 1);
    return true;
}

static bool radio_key(uint32_t rising)
{
    if (!is_radio_view()) return false;
    if (rising & (1U << 2)) {
        if (view == VIEW_CAPTURE) { request_radio(RF_CMD_STOP); view = VIEW_RECEIVE; }
        else if (view == VIEW_MONITOR_RUNNING) { request_radio(RF_CMD_STOP); view = VIEW_MONITOR; }
        else if (view == VIEW_SIGNAL) view = VIEW_LIBRARY;
        else if (view == VIEW_DELETE) view = VIEW_SIGNAL;
        else { request_radio(RF_CMD_STOP); view = VIEW_MENU; }
        change_view(TRANSITION_HORIZONTAL, -1); return true;
    }
    if ((rising & (1U << 1)) && view == VIEW_LIBRARY) {
        if (!radio_status.library_count) return true;
        selected_signal_id = radio_status.library[library_page].id;
        signal_action = 0; view = VIEW_SIGNAL;
        change_view(TRANSITION_HORIZONTAL, 1); return true;
    }
    if (rising & (1U << 3)) {
        if (radio_waiting) return true;
        if (view == VIEW_RECEIVE) { request_radio(RF_CMD_RECEIVE); view = VIEW_CAPTURE; radio_details = false; }
        else if (view == VIEW_MONITOR) { request_radio(RF_CMD_MONITOR); view = VIEW_MONITOR_RUNNING; radio_details = false; }
        else if (view == VIEW_CAPTURE) {
            if (radio_status.state == RF_CAPTURED) request_radio(RF_CMD_SAVE);
            else if (radio_status.state == RF_SAVED || radio_status.state == RF_FAILURE) request_radio(RF_CMD_RECEIVE);
            else { request_radio(RF_CMD_STOP); view = VIEW_RECEIVE; }
        }
        else if (view == VIEW_LIBRARY) {
            if (!radio_status.library_count) { request_radio(RF_CMD_LIBRARY); return true; }
            selected_signal_id = radio_status.library[library_page].id;
            request_radio(RF_CMD_SEND); set_row(&rows[active_row]); return true;
        }
        else if (view == VIEW_SIGNAL) {
            if (signal_action) view = VIEW_DELETE;
            else { request_radio(RF_CMD_SEND); set_row(&rows[active_row]); return true; }
        }
        else if (view == VIEW_DELETE) { request_radio(RF_CMD_DELETE); view = VIEW_LIBRARY; }
        else { request_radio(RF_CMD_STOP); view = VIEW_MONITOR; }
        change_view(TRANSITION_HORIZONTAL, 1); return true;
    }
    uint32_t vertical = rising & ((1U << 0) | (1U << 4));
    if (!vertical || vertical == ((1U << 0) | (1U << 4))) return true;
    int direction = rising & (1U << 0) ? -1 : 1;
    if (view == VIEW_RECEIVE || view == VIEW_MONITOR) frequency_page = (frequency_page + FREQUENCY_COUNT + direction) % FREQUENCY_COUNT;
    else if (view == VIEW_LIBRARY && radio_status.library_count) {
        unsigned old_page = library_page / SIGNALS_PER_PAGE;
        library_page = (library_page + radio_status.library_count + direction) % radio_status.library_count;
        if (old_page == library_page / SIGNALS_PER_PAGE) {
            settle_transition(); set_row(&rows[active_row]); return true;
        }
    }
    else if (view == VIEW_SIGNAL) signal_action = !signal_action;
    else if (view == VIEW_CAPTURE || view == VIEW_MONITOR_RUNNING) radio_details = !radio_details;
    else return true;
    change_view(TRANSITION_VERTICAL, direction); return true;
}

uint8_t rf_ui_get_brightness(void) { return brightness; }
void rf_ui_set_receive_options(uint8_t options) { receive_options = options & 3; if (view == VIEW_SETTINGS) set_row(&rows[active_row]); }
uint8_t rf_ui_get_receive_options(void) { return receive_options; }
uint32_t rf_ui_receive_frequency(void) { return frequencies[frequency_page]; }
void rf_ui_set_brightness(uint8_t value) { if (value >= 1 && value <= 100) { brightness = value; if (view == VIEW_BRIGHTNESS) set_row(&rows[active_row]); } }

void rf_ui_set_sleep_minutes(uint8_t light, uint8_t deep)
{
    if (light > 120 || deep > 120) return;
    sleep_minutes[0] = light; sleep_minutes[1] = deep;
    if (view == VIEW_SLEEP) set_row(&rows[active_row]);
}

void rf_ui_get_sleep_minutes(uint8_t *light, uint8_t *deep)
{
    *light = sleep_minutes[0]; *deep = sleep_minutes[1];
}

static void adjust_brightness(bool increase)
{
    uint8_t *value = view == VIEW_SLEEP ? &sleep_minutes[sleep_field] : &brightness;
    unsigned min = view == VIEW_SLEEP ? 0 : 1, max = view == VIEW_SLEEP ? 120 : 100;
    if (increase && *value < max) (*value)++;
    else if (!increase && *value > min) (*value)--;
    else return;
    set_row(&rows[active_row]);
}

static void brightness_repeat(lv_timer_t *timer)
{
    (void)timer;
    if ((view != VIEW_BRIGHTNESS && view != VIEW_SLEEP) || !repeat_key ||
        (previous_keys & ((1U << 0) | (1U << 4))) != repeat_key) {
        repeat_key = 0;
        return;
    }
    uint32_t now = lv_tick_get();
    uint32_t held = now - hold_started;
    if (held < 400) return;
    if (now - last_repeat < 120) return;
    last_repeat = now;
    adjust_brightness(repeat_key == (1U << 0));
}

void rf_ui_set_battery_mv(uint16_t millivolts)
{
    if (!millivolts) lv_label_set_text(battery_label, "--.--V");
    else lv_label_set_text_fmt(battery_label, "%u.%02uV", millivolts / 1000,
                               (millivolts % 1000) / 10);
}

void rf_ui_set_battery_raw(int raw)
{
    if (raw < 0) lv_label_set_text(battery_raw_label, "----");
    else lv_label_set_text_fmt(battery_raw_label, "%d", raw);
}

void rf_ui_create(void)
{
    page = 0;
    settings_page = 0;
    receive_options = 0;
    view = VIEW_MENU;
    brightness = 70;
    sleep_minutes[0] = 10; sleep_minutes[1] = 20; sleep_field = 0;
    active_row = 0;
    previous_keys = 0;
    repeat_key = 0;
    debug_state = RF_DEBUG_OFF;
    debug_request = false;
    memset(&radio_status, 0, sizeof(radio_status));
    radio_action.type = RF_CMD_NONE; frequency_page = library_page = signal_action = 0;
    selected_signal_id = 0;
    radio_details = radio_waiting = false;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0C0D11), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x1B1B22), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_t *battery = lv_obj_create(screen);
    lv_obj_remove_style_all(battery);
    lv_obj_set_size(battery, 46, 15);
    lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -3, 2);
    lv_obj_remove_flag(battery, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *body = lv_obj_create(battery);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 42, 15);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(0x949DAA), 0);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *terminal = lv_obj_create(battery);
    lv_obj_remove_style_all(terminal);
    lv_obj_set_size(terminal, 3, 7);
    lv_obj_set_pos(terminal, 42, 4);
    lv_obj_set_style_bg_color(terminal, lv_color_hex(0x949DAA), 0);
    lv_obj_set_style_bg_opa(terminal, LV_OPA_COVER, 0);
    battery_label = lv_label_create(body);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(0xDDE2E7), 0);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_10, 0);
    lv_obj_center(battery_label);
    rf_ui_set_battery_mv(0);

    lv_obj_t *viewport = lv_obj_create(screen);
    lv_obj_remove_style_all(viewport);
    lv_obj_set_size(viewport, 160, ROW_HEIGHT);
    lv_obj_set_pos(viewport, 0, 18);
    lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    for (unsigned i = 0; i < 2; i++) {
        rows[i].box = lv_obj_create(viewport);
        lv_obj_remove_style_all(rows[i].box);
        lv_obj_set_size(rows[i].box, 156, ROW_HEIGHT);
        lv_obj_set_pos(rows[i].box, 2, 0);
        lv_obj_set_style_bg_opa(rows[i].box, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_dir(rows[i].box, LV_GRAD_DIR_HOR, 0);
        lv_obj_set_style_radius(rows[i].box, 8, 0);
        lv_obj_set_style_border_width(rows[i].box, 1, 0);
        lv_obj_add_event_cb(rows[i].box, debug_button_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_remove_flag(rows[i].box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(rows[i].box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rows[i].box, LV_FLEX_ALIGN_CENTER,
                             LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(rows[i].box, 12, 0);
        rows[i].icon = lv_label_create(rows[i].box);
        lv_obj_set_style_text_font(rows[i].icon, &lv_font_montserrat_36, 0);
        rows[i].label = lv_label_create(rows[i].box);
        lv_obj_set_style_text_font(rows[i].label, &rf_font_menu_32, 0);
        rows[i].highlight = lv_obj_create(rows[i].box);
        lv_obj_remove_style_all(rows[i].highlight);
        lv_obj_add_flag(rows[i].highlight, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(rows[i].highlight, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(rows[i].highlight, 3, 1);
        lv_obj_set_size(rows[i].highlight, 148, 15);
        lv_obj_set_style_bg_color(rows[i].highlight, lv_color_hex(0x453326), 0);
        lv_obj_set_style_bg_opa(rows[i].highlight, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(rows[i].highlight, 3, 0);
        for (unsigned j = 0; j < SIGNALS_PER_PAGE; j++) {
            lv_obj_t *label = rows[i].signals[j] = lv_label_create(rows[i].box);
            lv_obj_add_flag(label, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_event_cb(label, signal_clicked, LV_EVENT_CLICKED, NULL);
            lv_obj_set_pos(label, 3, 1 + j * 15);
            lv_obj_set_size(label, 148, 15);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        }
    }
    lv_obj_add_flag(rows[1].box, LV_OBJ_FLAG_HIDDEN);
    page_label = lv_label_create(screen);
    lv_obj_set_width(page_label, 154);
    lv_label_set_long_mode(page_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(page_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(page_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(page_label, lv_color_hex(0x9BAAB5), 0);
    lv_obj_align(page_label, LV_ALIGN_BOTTOM_MID, 0, -2);
    set_row(&rows[0]);
    battery_raw_label = lv_label_create(screen);
    lv_obj_set_style_text_color(battery_raw_label, lv_color_hex(0xDDE2E7), 0);
    lv_obj_set_style_text_font(battery_raw_label, &lv_font_montserrat_10, 0);
    lv_obj_set_width(battery_raw_label, 62);
    lv_obj_set_style_text_align(battery_raw_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(battery_raw_label, LV_ALIGN_TOP_RIGHT, -53, 4);
    rf_ui_set_battery_raw(-1);
    lv_timer_create(brightness_repeat, 10, NULL);
}

void rf_ui_set_keys(uint32_t mask)
{
    uint32_t rising = mask & ~previous_keys;
    previous_keys = mask;
    const uint32_t up = 1U << 0, down = 1U << 4;
    if ((view == VIEW_MENU || view == VIEW_SETTINGS) &&
        (rising & (1U << 1)) && !(mask & (1U << 2))) rising |= 1U << 3;
    if (radio_key(rising)) return;
    if (view == VIEW_MENU && (rising & (1U << 3)) && page < 3) {
        repeat_key = 0; radio_details = radio_waiting = false;
        view = page == 0 ? VIEW_RECEIVE : page == 1 ? VIEW_LIBRARY : VIEW_MONITOR;
        if (page == 1) request_radio(RF_CMD_LIBRARY);
        change_view(TRANSITION_HORIZONTAL, 1); return;
    }
    if ((rising & (1U << 2)) && view != VIEW_MENU) {
        repeat_key = 0;
        view = (view == VIEW_BRIGHTNESS || view == VIEW_SLEEP) ? VIEW_SETTINGS : VIEW_MENU;
        change_view(TRANSITION_HORIZONTAL, -1);
        return;
    }
    if (view == VIEW_SLEEP && (rising & ((1U << 3) | (1U << 1)))) {
        repeat_key = 0; sleep_field ^= 1;
        settle_transition(); set_row(&rows[active_row]); return;
    }
    if (rising & (1U << 3)) {
        repeat_key = 0;
        if (view == VIEW_DEBUG) { request_debug_start(); return; }
        if (view == VIEW_MENU && page == 3) {
            view = VIEW_DEBUG;
            change_view(TRANSITION_HORIZONTAL, 1);
            return;
        }
        bool returning = (view == VIEW_BRIGHTNESS);
        if (view == VIEW_MENU && page == MENU_COUNT - 1) {
            view = VIEW_SETTINGS;
            settings_page = 0;
        }
        else if (view == VIEW_SETTINGS && settings_page == 0) view = VIEW_BRIGHTNESS;
        else if (view == VIEW_SETTINGS && settings_page == 1) { view = VIEW_SLEEP; sleep_field = 0; }
        else if (view == VIEW_SETTINGS && settings_page >= 5) {
            receive_options ^= 1U << (settings_page-5);
            settle_transition(); set_row(&rows[active_row]); return;
        }
        else if ((view == VIEW_BRIGHTNESS)) view = VIEW_SETTINGS;
        else return;
        change_view(TRANSITION_HORIZONTAL, returning ? -1 : 1);
        return;
    }
    if (!(mask & (up | down)) || (mask & (up | down)) == (up | down)) repeat_key = 0;
    if ((mask & (up | down)) == (up | down)) return;
    if (view == VIEW_DEBUG) return;
    if (view == VIEW_BRIGHTNESS || view == VIEW_SLEEP) {
        if (rising & up) repeat_key = up;
        else if (rising & down) repeat_key = down;
        else return;
        hold_started = last_repeat = lv_tick_get();
        adjust_brightness(repeat_key == up);
        return;
    }
    int direction;
    if (rising & up) direction = -1;
    else if (rising & down) direction = 1;
    else return;
    unsigned *selected = view == VIEW_SETTINGS ? &settings_page : &page;
    unsigned count = view == VIEW_SETTINGS ? SETTINGS_COUNT : MENU_COUNT;
    *selected = direction < 0 ? (*selected + count - 1) % count : (*selected + 1) % count;
    change_view(TRANSITION_VERTICAL, direction);
}
