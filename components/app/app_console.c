#include "app_internal.h"
#include "rf_ui.h"
#include "rf_service.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include <string.h>

// Callbacks run in the LVGL owner task, never in a timer service or ISR.
static void serial_command(const char *line)
{
    uint32_t hz;
    bool start = true;
    if (!strcmp(line, "rx 433")) hz = 433920000;
    else if (!strcmp(line, "rx 315")) hz = 315000000;
    else if (!strcmp(line, "rx 318")) hz = 318000000;
    else if (!strcmp(line, "rx auto")) hz = 0;
    else if (!strcmp(line, "stop")) { hz = 0; start = false; }
    else if (!strcmp(line, "status")) {
        rf_status_t state;
        rf_service_status(&state);
        ESP_LOGI("serial", "STATUS state=%u hz=%lu RSSI=%d raw=%u bits=%u pulses=%u code=%s error=%s",
                 (unsigned)state.state, (unsigned long)state.frequency_hz, state.rssi_dbm,
                 (unsigned)state.raw, (unsigned)state.bits, (unsigned)state.pulse_count,
                 state.code, state.error);
        return;
    } else {
        ESP_LOGI("serial", "Commands: rx 433 | rx 315 | rx 318 | rx auto | stop | status");
        return;
    }
    if (rf_ui_remote_receive(hz, start)) {
        app_radio_sync(NULL);
        ESP_LOGI("serial", "Requested %s %lu Hz", start ? "RX" : "STOP", (unsigned long)hz);
    }
}

void app_console_poll(lv_timer_t *timer)
{
    (void)timer;
    static char line[48];
    static unsigned used;
    static bool overflow;
    char data[64];
    int count = usb_serial_jtag_read_bytes(data, sizeof(data), 0);
    for (int i = 0; i < count; i++) {
        unsigned char c = data[i];
        app_power_activity();
        if (c == '\r' || c == '\n') {
            line[used] = 0;
            if (overflow) ESP_LOGW("serial", "Command too long; discarded");
            else if (used) serial_command(line);
            used = 0; overflow = false;
        } else if (c == 8 || c == 127) {
            if (used && !overflow) used--;
        } else if (c >= 32 && c <= 126) {
            if (used < sizeof(line)-1 && !overflow) line[used++] = c;
            else overflow = true;
        }
    }
}

