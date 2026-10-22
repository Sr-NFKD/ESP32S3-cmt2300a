#include "rf_service.h"
#include "bsp_radio.h"
#include "bsp_board.h"
#include "user_data.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Four 24-bit frames fit; even 32-bit/1 ms PWM fills this block within
// the 1 s scan lock. TX still needs the full RF_MAX_PULSES capacity.
#define RX_SYMBOLS 128
// ESP-IDF 5.5 uses at least two DMA descriptors and splits this buffer
// equally. Reserve two blocks so each partial callback contains 128 symbols.
#define RX_DMA_SYMBOLS (RX_SYMBOLS * 2)
#define RX_IDLE_US 32000
static uint32_t rx_blocks, rx_full_blocks, rx_decoded_blocks;
// Microseconds; tune on hardware for the target remote's repeat period/noise floor.
#define SCAN_DWELL_US 2000
#define SCAN_HOLD_US 250000
#define SCAN_MAX_LOCK_US 1000000
#define SCAN_RSSI_DBM (-92)
#define SCAN_DELTA_DB 12
#define SCAN_COOLDOWN_US 400000
#define CAL_MAX_AGE_US 10000000
// Adapted scan strategy: manhere/esp32s3-cmt2300a-spectrum, see THIRD_PARTY_NOTICES.md.
// Only CC1101-supported presets in the project's low/433 MHz bands.
static const uint32_t scan_frequencies[] = {
    433920000, 315000000, 318000000, 303000000, 306000000, 310000000,
    330000000, 390000000, 418000000, 430500000, 431500000, 432800000, 434500000,
};
#define SCAN_COUNT (sizeof(scan_frequencies)/sizeof(scan_frequencies[0]))
typedef struct {
    float floor_dbm;
    bool baseline_valid, calibration_valid;
    uint8_t calibration[3];
    int64_t calibrated_at, cooldown_until;
} scan_channel_t;
static scan_channel_t scan_channels[SCAN_COUNT];
static unsigned scan_index;
static unsigned fallback_index;
static bool fallback_used;
static int scan_best = -1;
static int16_t scan_best_rssi = -127;
static int64_t signal_at, locked_at;
static bool profile_loaded;
static const char *TAG = "rf_service";
static QueueHandle_t commands, received;
static SemaphoreHandle_t status_lock, paused;
static rf_status_t status;
static rmt_channel_handle_t rx_channel, tx_channel;
static rmt_encoder_handle_t encoder;
static rmt_symbol_word_t *rx_symbols, *tx_symbols;
static rmt_symbol_word_t *rx_snapshot;
static atomic_bool rx_event_claimed;
static bool rx_snapshot_partial;
static rf_frame_t *capture, *candidate;
static bool radio_ready, receiving, rx_enabled, tx_enabled, monitoring, have_candidate;
static bool auto_frequency;
static int64_t tuned_at, last_rssi, last_monitor_publish;
static uint32_t tuned_frequency, library_revision;
static unsigned samples, occupied;
static int16_t peak_rssi;
static volatile bool rx_overflow;
static atomic_bool ota_locked;
static TaskHandle_t worker_task;
static esp_timer_handle_t scan_timer;

static void publish(rf_state_t state, esp_err_t err)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.state = state;
    if (err != ESP_OK) snprintf(status.error, sizeof(status.error), "%s", esp_err_to_name(err));
    else status.error[0] = 0;
    status.frequency_hz = tuned_frequency;
    status.revision++;
    xSemaphoreGive(status_lock);
}

static esp_err_t command(uint8_t strobe) { return bsp_radio_strobe(strobe, NULL); }
static esp_err_t write_reg(uint8_t address, uint8_t value) { return bsp_radio_write_reg(address, value); }

static esp_err_t enter_state(uint8_t strobe, uint8_t expected)
{
    esp_err_t err = command(strobe);
    int64_t deadline = esp_timer_get_time() + 50000;
    while (err == ESP_OK && esp_timer_get_time() < deadline) {
        uint8_t marc;
        err = bsp_radio_read_reg(0x35, &marc);
        if (err == ESP_OK && (marc & 0x1f) == expected) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    command(0x36);
    return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
}

static bool supported_frequency(uint32_t hz)
{
    for (unsigned i = 0; i < sizeof(scan_frequencies)/sizeof(scan_frequencies[0]); i++)
        if (hz == scan_frequencies[i]) return true;
    return false;
}

static bool scan_trigger(scan_channel_t *channel, int16_t rssi, int64_t now)
{
    if (!channel->baseline_valid) {
        channel->floor_dbm = rssi;
        channel->baseline_valid = true;
        return rssi >= -65 && now >= channel->cooldown_until;
    }
    bool hit = rssi >= -65 || (rssi >= SCAN_RSSI_DBM && rssi - channel->floor_dbm >= SCAN_DELTA_DB);
    // Fast downward / slow upward baseline, as in the reference sniff strategy.
    channel->floor_dbm += (rssi < channel->floor_dbm ? 0.30f : 0.01f) * (rssi - channel->floor_dbm);
    return hit && now >= channel->cooldown_until;
}

static bool scan_due(int64_t now)
{
    if (!rx_enabled) return now - tuned_at >= SCAN_DWELL_US;
    return now - locked_at >= SCAN_MAX_LOCK_US || now - signal_at >= SCAN_HOLD_US;
}

static bool scan_should_capture(scan_channel_t *channel, int16_t rssi, int64_t now)
{
    bool hit = scan_trigger(channel, rssi, now);
    // One ungated capture per sweep, rotating across frequencies. A signal
    // present during baseline learning must never remain permanently invisible.
    bool fallback = !fallback_used && scan_index == fallback_index && now >= channel->cooldown_until;
    if (fallback) {
        fallback_used = true;
        fallback_index = (fallback_index + 1) % SCAN_COUNT;
    }
    return hit || fallback;
}

static void scan_consider(int16_t rssi, int64_t now)
{
    if (scan_should_capture(&scan_channels[scan_index], rssi, now) &&
        (scan_best < 0 || rssi > scan_best_rssi)) {
        scan_best = scan_index; scan_best_rssi = rssi;
    }
}

static unsigned scan_next(bool was_locked, bool can_capture, bool *lock_best)
{
    *lock_best = can_capture && !was_locked && scan_index == SCAN_COUNT-1 && scan_best >= 0;
    unsigned next = was_locked ? 0 : (scan_index + 1) % SCAN_COUNT;
    if (*lock_best) next = (unsigned)scan_best;
    if (was_locked || *lock_best || next == 0) {
        fallback_used = false; scan_best = -1; scan_best_rssi = -127;
    }
    return next;
}

static esp_err_t tune(uint32_t hz, bool transmit)
{
    if (!supported_frequency(hz)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = enter_state(0x36, 0x01); // Confirm SIDLE before changing synthesizer/profile.
    // TI asynchronous OOK starting profile; no sync/CRC/packet engine.
    static const uint8_t profile[][2] = {
        {0x00,0x0d},{0x02,0x2e},{0x07,0x00},{0x08,0x32},{0x0b,0x06},{0x0c,0x00},
        {0x10,0x87},{0x11,0x32},{0x12,0x30},{0x13,0x00},{0x14,0x00},
        {0x16,0x07},{0x17,0x00},{0x18,0x18},{0x19,0x18},{0x1b,0x04},{0x1c,0x00},{0x1d,0x92},
        {0x21,0xb6},{0x22,0x11},{0x2c,0x81},{0x2d,0x35},{0x2e,0x09},
    };
    if (!profile_loaded) {
        for (unsigned i = 0; err == ESP_OK && i < sizeof(profile)/sizeof(profile[0]); i++)
            err = write_reg(profile[i][0], profile[i][1]);
        profile_loaded = err == ESP_OK;
    }
    uint32_t word = ((uint64_t)hz * 65536 + BSP_RADIO_XTAL_HZ / 2) / BSP_RADIO_XTAL_HZ;
    const uint8_t frequency[3] = {word >> 16, word >> 8, word};
    if (err == ESP_OK) err = bsp_radio_write_burst(0x0d, frequency, sizeof(frequency));
    unsigned index = 0;
    while (index < SCAN_COUNT && scan_frequencies[index] != hz) index++;
    scan_channel_t *channel = &scan_channels[index]; // supported_frequency checked above.
    bool cached = !transmit && channel->calibration_valid &&
        esp_timer_get_time() - channel->calibrated_at < CAL_MAX_AGE_US;
    if (err == ESP_OK) err = write_reg(0x18, cached ? 0x08 : 0x18);
    if (err == ESP_OK && cached)
        err = bsp_radio_write_burst(0x23, channel->calibration, 3);
    if (err == ESP_OK && !cached) {
        // Reset calibration seeds when changing bands; fresh IDLE->RX/TX autocal.
        const uint8_t seeds[] = {0xe9, 0x2a, 0x00, 0x1f};
        err = bsp_radio_write_burst(0x23, seeds, sizeof(seeds));
    }
    if (err == ESP_OK && transmit) {
        const uint8_t pa[8] = {0, hz < 348000000 ? 0x51 : 0x60};
        err = bsp_radio_write_burst(0x3e, pa, sizeof(pa));
    }
    if (err == ESP_OK) err = enter_state(transmit ? 0x35 : 0x34, transmit ? 0x13 : 0x0d);
    if (err == ESP_OK && !cached && !transmit) {
        err = bsp_radio_read_burst(0x23, channel->calibration, 3);
        channel->calibration_valid = err == ESP_OK;
        channel->calibrated_at = esp_timer_get_time();
    }
    if (err == ESP_OK) { tuned_frequency = hz; tuned_at = esp_timer_get_time(); }
    else {
        profile_loaded = false;
        for (unsigned i = 0; i < SCAN_COUNT; i++) scan_channels[i].calibration_valid = false;
    }
    return err;
}

static bool rx_done(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *event, void *context)
{
    (void)channel; (void)context;
    BaseType_t wake = pdFALSE;
    // DMA full-block callbacks do not stop reception. Keep only the first
    // block, copying it before DMA can reuse its buffer. The worker stops
    // the transaction before decoding and explicitly starts the next one.
    if (atomic_exchange(&rx_event_claimed, true)) return false;
    size_t count = event->num_symbols;
    rx_snapshot_partial = !event->flags.is_last;
    if (count <= RX_SYMBOLS)
        memcpy(rx_snapshot, event->received_symbols, count * sizeof(*rx_snapshot));
    if (xQueueSendFromISR(received, &count, &wake) != pdTRUE) rx_overflow = true;
    if (worker_task) vTaskNotifyGiveFromISR(worker_task, &wake);
    return wake == pdTRUE;
}

static esp_err_t arm_receive(void)
{
    rx_overflow = false;
    atomic_store(&rx_event_claimed, false);
    peak_rssi = -127; // Strength belongs to this capture, never an older burst.
    const rmt_receive_config_t config = {
        .signal_range_min_ns = 1000, // Hardware filter is clocked at APB, not the 1 us capture tick.
        .signal_range_max_ns = RX_IDLE_US * 1000,
        .flags.en_partial_rx = true,
    };
    return rmt_receive(rx_channel, rx_symbols, RX_DMA_SYMBOLS * sizeof(*rx_symbols), &config);
}

static void stop_radio(void)
{
    receiving = monitoring = false;
    if (rx_enabled) { rmt_disable(rx_channel); rx_enabled = false; }
    if (tx_enabled) { rmt_disable(tx_channel); tx_enabled = false; }
    if (radio_ready) command(0x36);
    if (received) xQueueReset(received);
}

static void refresh_library(void)
{
    unsigned read_revision = user_signals_revision();
    cJSON *records = NULL;
    esp_err_t err = user_signals_read(&records);
    if (err != ESP_OK) { publish(RF_FAILURE, err); return; }
    xSemaphoreTake(status_lock, portMAX_DELAY);
    memset(status.library, 0, sizeof(status.library));
    status.library_count = 0;
    cJSON *record;
    cJSON_ArrayForEach(record, records) {
        if (status.library_count >= RF_LIBRARY_LIMIT) break;
        cJSON *id = cJSON_GetObjectItemCaseSensitive(record, "id");
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "key_code"));
        if (!cJSON_IsNumber(id) || id->valuedouble < 1 || !code) continue;
        rf_library_item_t *item = &status.library[status.library_count++];
        item->id = (uint32_t)id->valuedouble;
        cJSON *hz = cJSON_GetObjectItemCaseSensitive(record, "frequency_hz");
        item->frequency_hz = cJSON_IsNumber(hz) ? (uint32_t)hz->valuedouble : 0;
        snprintf(item->code, sizeof(item->code), "%.16s", code);
        const char *protocol = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "protocol"));
        item->raw = protocol && !strcmp(protocol, "RAW");
        if (item->raw) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "name"));
            const char *suffix = name ? strstr(name, "_RAW-") : NULL;
            suffix = suffix ? suffix + 5 : NULL;
            if (suffix && *suffix && strlen(suffix) < sizeof(item->code) &&
                strspn(suffix, "0123456789") == strlen(suffix))
                snprintf(item->code, sizeof(item->code), "%s", suffix);
            else snprintf(item->code, sizeof(item->code), "%lu", (unsigned long)item->id);
        }
    }
    status.revision++;
    xSemaphoreGive(status_lock);
    cJSON_Delete(records);
    library_revision = read_revision; // A concurrent web write must trigger another refresh.
}

static esp_err_t save_capture(void)
{
    if (!have_candidate) return ESP_ERR_INVALID_STATE;
    cJSON *record = cJSON_CreateObject(), *raw = cJSON_CreateArray();
    if (!record || !raw) { cJSON_Delete(record); cJSON_Delete(raw); return ESP_ERR_NO_MEM; }
    char code[17], name[40];
    if (candidate->bits) snprintf(code, sizeof(code), "%0*lX", (candidate->bits+3)/4, (unsigned long)candidate->code);
    else snprintf(code, sizeof(code), "%s", candidate->raw_code[0] ? candidate->raw_code : "0");
    if (candidate->bits)
        snprintf(name, sizeof(name), "%s", code + (strlen(code) > 6 ? strlen(code) - 6 : 0));
    else
        snprintf(name, sizeof(name), "%lu.%02lu_RAW-%lu", (unsigned long)(candidate->frequency_hz / 1000000),
                 (unsigned long)(candidate->frequency_hz / 10000 % 100), (unsigned long)(esp_timer_get_time()/1000));
    bool ok = cJSON_AddStringToObject(record, "name", name) &&
        cJSON_AddStringToObject(record, "key_code", code) &&
        cJSON_AddStringToObject(record, "protocol", candidate->bits ? "OOK_PWM" : "RAW") &&
        cJSON_AddStringToObject(record, "modulation", "OOK") &&
        cJSON_AddNumberToObject(record, "frequency_hz", candidate->frequency_hz) &&
        cJSON_AddNumberToObject(record, "bits", candidate->bits) &&
        cJSON_AddNumberToObject(record, "pulse_us", candidate->pulse_us) &&
        cJSON_AddNumberToObject(record, "first_level", candidate->first_level) &&
        cJSON_AddNumberToObject(record, "repeats", 5);
    for (unsigned i = 0; ok && i < candidate->count; i++) {
        cJSON *value = cJSON_CreateNumber(candidate->duration[i]);
        if (!value || !cJSON_AddItemToArray(raw, value)) { cJSON_Delete(value); ok = false; }
    }
    if (!ok || !cJSON_AddItemToObject(record, "raw", raw)) {
        cJSON_Delete(raw); cJSON_Delete(record); return ESP_ERR_NO_MEM;
    }
    uint32_t id;
    esp_err_t err = user_signal_save(record, &id);
    cJSON_Delete(record);
    if (err == ESP_OK) {
        xSemaphoreTake(status_lock, portMAX_DELAY); status.saved_id = id; xSemaphoreGive(status_lock);
        have_candidate = false; refresh_library();
    }
    return err;
}

static esp_err_t load_frame(uint32_t id, rf_frame_t *frame)
{
    cJSON *records = NULL;
    esp_err_t err = user_signals_read(&records);
    if (err != ESP_OK) return err;
    err = ESP_ERR_NOT_FOUND;
    cJSON *record;
    cJSON_ArrayForEach(record, records) {
        cJSON *number = cJSON_GetObjectItemCaseSensitive(record, "id");
        if (!cJSON_IsNumber(number) || number->valuedouble != id) continue;
        err = ESP_ERR_INVALID_ARG;
        cJSON *hz = cJSON_GetObjectItemCaseSensitive(record, "frequency_hz");
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "protocol"));
        if (!cJSON_IsNumber(hz) || !supported_frequency(hz->valuedouble) || !kind) break;
        memset(frame, 0, sizeof(*frame)); frame->frequency_hz = hz->valuedouble;
        if (!strcmp(kind, "OOK_PWM")) {
            cJSON *bits = cJSON_GetObjectItemCaseSensitive(record, "bits");
            cJSON *pulse = cJSON_GetObjectItemCaseSensitive(record, "pulse_us");
            uint32_t code;
            if (cJSON_IsNumber(bits) && cJSON_IsNumber(pulse) &&
                rf_code_parse(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record,"key_code")), bits->valueint, &code) &&
                rf_frame_encode(frame, code, bits->valueint, pulse->valueint)) err = ESP_OK;
        } else if (!strcmp(kind, "RAW")) {
            cJSON *raw = cJSON_GetObjectItemCaseSensitive(record, "raw");
            cJSON *first = cJSON_GetObjectItemCaseSensitive(record, "first_level");
            unsigned count = cJSON_GetArraySize(raw), total = 0;
            if (!cJSON_IsArray(raw) || count < 16 || count > RF_MAX_PULSES ||
                !cJSON_IsNumber(first) || (first->valueint != 0 && first->valueint != 1)) break;
            frame->count = count; frame->first_level = first->valueint; err = ESP_OK;
            for (unsigned i = 0; i < count; i++) {
                cJSON *duration = cJSON_GetArrayItem(raw, i);
                if (!cJSON_IsNumber(duration) || duration->valuedouble < 1 || duration->valuedouble > 32767 ||
                    duration->valuedouble != duration->valueint) { err = ESP_ERR_INVALID_ARG; break; }
                frame->duration[i] = duration->valueint; total += duration->valueint;
            }
            if (total > 300000) err = ESP_ERR_INVALID_SIZE;
        }
        break;
    }
    cJSON_Delete(records);
    return err;
}

static esp_err_t transmit(uint32_t id)
{
    esp_err_t err = load_frame(id, capture);
    if (err != ESP_OK) return err;
    stop_radio();
    err = rmt_enable(tx_channel);
    if (err == ESP_OK) { tx_enabled = true; err = tune(capture->frequency_hz, true); }
    if (err != ESP_OK) { stop_radio(); return err; }
    // Pad an odd last duration with low idle. A bounded burst, never a continuous carrier.
    unsigned count = (capture->count + 1) / 2;
    for (unsigned i = 0; i < count; i++) {
        unsigned index = i * 2;
        tx_symbols[i] = (rmt_symbol_word_t){
            .level0 = capture->first_level,
            .duration0 = capture->duration[index],
            .level1 = index + 1 < capture->count ? !capture->first_level : 0,
            .duration1 = index + 1 < capture->count ? capture->duration[index + 1] : 1000,
        };
    }
    publish(RF_SENDING, ESP_OK);
    const rmt_transmit_config_t config = {.loop_count = 0, .flags.eot_level = 0};
    for (unsigned repeat = 0; repeat < 5 && err == ESP_OK; repeat++) {
        err = rmt_transmit(tx_channel, encoder, tx_symbols, count * sizeof(*tx_symbols), &config);
        if (err == ESP_OK) err = rmt_tx_wait_all_done(tx_channel, 400);
        // A stop request can interrupt between frames; transmission is always bounded.
        rf_command_t pending;
        if (xQueuePeek(commands, &pending, 0) == pdTRUE && pending.type == RF_CMD_STOP) break;
    }
    stop_radio();
    return err;
}

static void read_capture(size_t symbols)
{
    rx_blocks++;
    if (rx_overflow || symbols > RX_SYMBOLS) return;
    bool full = rx_snapshot_partial || symbols == RX_SYMBOLS;
    if (full) rx_full_blocks++;
    memset(capture, 0, sizeof(*capture)); capture->frequency_hz = tuned_frequency;
    for (unsigned i = 0; i < symbols; i++) {
        const uint16_t durations[2] = {rx_snapshot[i].duration0, rx_snapshot[i].duration1};
        const uint8_t levels[2] = {rx_snapshot[i].level0, rx_snapshot[i].level1};
        for (unsigned n = 0; n < 2; n++) {
            if (!durations[n]) continue;
            unsigned count = capture->count;
            if (!count) capture->first_level = levels[n];
            else if ((capture->first_level ^ ((count - 1) & 1)) == levels[n]) {
                unsigned sum = capture->duration[count - 1] + durations[n];
                if (sum > 32767) return;
                capture->duration[count - 1] = sum; continue;
            }
            if (capture->count >= RF_MAX_PULSES) return;
            capture->duration[capture->count++] = durations[n];
        }
    }
    if (capture->count) capture->tail_clipped = full || capture->duration[capture->count - 1] >= RX_IDLE_US - 100;
    if (!rf_frame_analyze(capture)) return;
    if (capture->bits || capture->raw_code[0]) rx_decoded_blocks++;
    if (!capture->bits) {
        // A full buffer can contain complete decoded frames, but is not a
        // complete RAW recording. Never save truncated unknown waveforms.
        if (full && capture->repeats < 2 && !capture->raw_code[0]) return;
        for (unsigned i = 0; i < capture->count; i++) if (capture->duration[i] < 60) return;
        if (peak_rssi < -85) return; // Suppress weak idle noise; RAW still needs explicit user confirmation.
        uint32_t total = 0;
        for (unsigned i = 0; i < capture->count; i++) total += capture->duration[i];
        if (total > 300000) return; // Invalid/noisy burst is discarded, not a session failure.
    }
    unsigned repeats = capture->repeats ? capture->repeats : 1;
    signal_at = esp_timer_get_time(); // Keep this frequency for the next repeated frame.
    if (have_candidate && rf_frame_same(capture, candidate)) repeats++;
    memcpy(candidate, capture, sizeof(*candidate)); have_candidate = true;
    // Unknown RAW must also recur; a single random OOK noise burst is not a signal.
    if (repeats < 2) return;
    stop_radio();
    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.last_signal_us = signal_at;
    status.raw = !candidate->bits; status.bits = candidate->bits; status.pulse_count = candidate->count;
    if (candidate->bits) snprintf(status.code, sizeof(status.code), "%0*lX", (candidate->bits+3)/4, (unsigned long)candidate->code);
    else if (candidate->raw_code[0]) snprintf(status.code, sizeof(status.code), "%s", candidate->raw_code);
    else snprintf(status.code, sizeof(status.code), "RAW %u", candidate->count);
    xSemaphoreGive(status_lock);
    publish(RF_CAPTURED, ESP_OK);
}

static void handle(const rf_command_t *request)
{
    if (request->type == RF_CMD_LIBRARY) { refresh_library(); return; }
    if (request->type == RF_CMD_STOP) { stop_radio(); publish(RF_IDLE, ESP_OK); xSemaphoreGive(paused); return; }
    if (request->type == RF_CMD_DELETE) {
        esp_err_t err = user_signal_delete(request->id); refresh_library(); publish(err == ESP_OK ? RF_IDLE : RF_FAILURE, err); return;
    }
    if (request->type == RF_CMD_SAVE) {
        esp_err_t err = save_capture();
        publish(err == ESP_OK ? RF_SAVED : RF_FAILURE, err);
        if (err == ESP_OK && auto_frequency && !request->keep_result) {
            const rf_command_t next = {.type = RF_CMD_RECEIVE, .frequency_hz = 0};
            handle(&next);
        }
        return;
    }
    if (!radio_ready) { publish(RF_FAILURE, ESP_ERR_NOT_FOUND); return; }
    if (request->type == RF_CMD_SEND) {
        esp_err_t err = transmit(request->id); publish(err == ESP_OK ? RF_SENT : RF_FAILURE, err); return;
    }
    stop_radio();
    have_candidate = false;
    auto_frequency = request->frequency_hz == 0;
    scan_index = 0; signal_at = 0;
    fallback_index = 0; fallback_used = false;
    scan_best = -1; scan_best_rssi = -127;
    if (!auto_frequency) {
        while (scan_index < SCAN_COUNT && scan_frequencies[scan_index] != request->frequency_hz) scan_index++;
        if (scan_index == SCAN_COUNT) { publish(RF_FAILURE, ESP_ERR_INVALID_ARG); return; }
    }
    for (unsigned i = 0; i < SCAN_COUNT; i++) {
        scan_channels[i].baseline_valid = false;
        scan_channels[i].cooldown_until = 0;
    }
    esp_err_t err = tune(auto_frequency ? scan_frequencies[scan_index] : request->frequency_hz, false);
    if (err == ESP_OK && request->type == RF_CMD_RECEIVE) {
        // Fixed-frequency reception must not depend on an RSSI baseline learned
        // while the remote may already be transmitting. Validate captured frames.
        if (!auto_frequency) {
            err = rmt_enable(rx_channel);
            if (err == ESP_OK) { rx_enabled = true; err = arm_receive(); }
        }
        receiving = err == ESP_OK;
    } else monitoring = err == ESP_OK && request->type == RF_CMD_MONITOR;
    last_rssi = esp_timer_get_time(); samples = occupied = 0;
    peak_rssi = -127;
    if (err != ESP_OK) stop_radio();
    publish(err != ESP_OK ? RF_FAILURE : receiving ? RF_LISTENING : RF_MONITORING, err);
}

static void scan_tick(void *unused)
{
    (void)unused;
    if (worker_task) xTaskNotifyGive(worker_task);
}

static void log_capture(size_t symbols)
{
    static int64_t last_log;
    int64_t now = esp_timer_get_time();
    if (now - last_log < 500000 || symbols > RX_SYMBOLS) return;
    last_log = now;
    char pulses[1000];
    size_t used = 0;
    unsigned count = symbols < 64 ? symbols : 64;
    pulses[0] = 0;
    for (unsigned i = 0; i < count; i++) {
        int n = snprintf(pulses + used, sizeof(pulses) - used, " %u:%u/%u:%u",
                         rx_snapshot[i].level0, rx_snapshot[i].duration0,
                         rx_snapshot[i].level1, rx_snapshot[i].duration1);
        if (n < 0 || (size_t)n >= sizeof(pulses) - used) break;
        used += n;
    }
    ESP_LOGI(TAG, "PULSE %lu Hz symbols=%u partial=%u RSSI=%d level:us%s",
             (unsigned long)tuned_frequency, (unsigned)symbols,
             (unsigned)rx_snapshot_partial, peak_rssi, pulses);
}

static void worker(void *unused)
{
    (void)unused;
    int64_t last_diagnostics = 0;
    refresh_library();
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        rf_command_t request;
        while (xQueueReceive(commands, &request, 0) == pdTRUE) handle(&request);
        size_t symbols;
        if (receiving && rx_enabled && xQueueReceive(received, &symbols, 0) == pdTRUE) {
            // Partial callbacks leave RMT in RUN; calling rmt_receive directly
            // would fail. Stop DMA before releasing the snapshot for reuse.
            esp_err_t err = rmt_disable(rx_channel);
            if (err != ESP_OK) { stop_radio(); publish(RF_FAILURE, err); continue; }
            rx_enabled = false;
            xQueueReset(received);
            log_capture(symbols);
            read_capture(symbols);
            if (receiving) {
                err = rmt_enable(rx_channel);
                if (err == ESP_OK) { rx_enabled = true; err = arm_receive(); }
                if (err != ESP_OK) { stop_radio(); publish(RF_FAILURE, err); }
            }
        }
        int64_t now = esp_timer_get_time();
        if (receiving && now - last_diagnostics >= 2000000) {
            ESP_LOGI(TAG, "RX %lu Hz capture=%u blocks=%lu full=%lu decoded=%lu RSSI=%d",
                     (unsigned long)tuned_frequency, (unsigned)rx_enabled,
                     (unsigned long)rx_blocks, (unsigned long)rx_full_blocks,
                     (unsigned long)rx_decoded_blocks, peak_rssi);
            last_diagnostics = now;
        }
        bool sniff = receiving && auto_frequency && !rx_enabled;
        bool probe_ready = now - tuned_at >= SCAN_DWELL_US;
        if ((receiving || monitoring) && probe_ready &&
            ((auto_frequency && !rx_enabled) || now - last_rssi >= 10000)) {
            last_rssi = now;
            uint8_t value;
            esp_err_t err = bsp_radio_read_reg(0x34, &value);
            if (err == ESP_OK) {
                int16_t rssi = (int8_t)value / 2 - 74;
                if (rssi > peak_rssi) peak_rssi = rssi;
                samples++; if (rssi > -75) occupied++;
                xSemaphoreTake(status_lock, portMAX_DELAY);
                status.rssi_dbm = rssi; status.frequency_hz = tuned_frequency;
                status.occupancy = samples ? occupied * 100 / samples : 0;
                if (now - last_monitor_publish >= 200000) { status.revision++; last_monitor_publish = now; }
                xSemaphoreGive(status_lock);
                if (samples >= 100) { samples /= 2; occupied /= 2; }
                if (sniff) scan_consider(rssi, now);
            } else {
                // Never treat a failed SPI read as a weak/quiet channel.
                stop_radio(); publish(RF_FAILURE, err);
            }
        }
        now = esp_timer_get_time();
        if ((receiving || monitoring) && auto_frequency && scan_due(now)) {
            bool restart = receiving;
            bool was_locked = rx_enabled;
            if (rx_enabled) scan_channels[scan_index].cooldown_until = now + SCAN_COOLDOWN_US;
            bool lock_best;
            unsigned next = scan_next(was_locked, receiving, &lock_best);
            stop_radio();
            scan_index = next;
            esp_err_t err = tune(scan_frequencies[scan_index], false);
            if (err == ESP_OK && lock_best) {
                err = rmt_enable(rx_channel);
                if (err == ESP_OK) { rx_enabled = true; err = arm_receive(); }
                locked_at = signal_at = esp_timer_get_time();
            }
            receiving = err == ESP_OK && restart;
            monitoring = err == ESP_OK && !restart;
            have_candidate = false;
            if (!lock_best) signal_at = 0;
            samples = occupied = 0;
            last_rssi = esp_timer_get_time();
            peak_rssi = -127;
            if (err != ESP_OK) { stop_radio(); publish(RF_FAILURE, err); }
        }
        if (library_revision != user_signals_revision()) refresh_library();
    }
}

esp_err_t rf_service_init(bool available)
{
    if (commands) return ESP_ERR_INVALID_STATE;
    radio_ready = available;
    commands = xQueueCreate(8, sizeof(rf_command_t)); received = xQueueCreate(2, sizeof(size_t));
    status_lock = xSemaphoreCreateMutex(); paused = xSemaphoreCreateBinary();
    capture = heap_caps_calloc(1, sizeof(*capture), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    candidate = heap_caps_calloc(1, sizeof(*candidate), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!capture) capture = calloc(1, sizeof(*capture));
    if (!candidate) candidate = calloc(1, sizeof(*candidate));
    if (!commands || !received || !status_lock || !paused || !capture || !candidate) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    if (available) {
        rx_symbols = heap_caps_calloc(RX_DMA_SYMBOLS, sizeof(*rx_symbols), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        rx_snapshot = heap_caps_calloc(RX_SYMBOLS, sizeof(*rx_snapshot), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        tx_symbols = heap_caps_calloc(RF_MAX_PULSES / 2 + 1, sizeof(*tx_symbols), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!rx_symbols || !rx_snapshot || !tx_symbols) return ESP_ERR_NO_MEM;
        const rmt_rx_channel_config_t rx = {.gpio_num = BSP_RADIO_GDO2, .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 1000000, .mem_block_symbols = RX_DMA_SYMBOLS, .flags.with_dma = true};
        const rmt_tx_channel_config_t tx = {.gpio_num = BSP_RADIO_GDO0, .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 1000000, .mem_block_symbols = 1024, .trans_queue_depth = 1, .flags.with_dma = true};
        err = write_reg(0x02, 0x2e); // GDO0 must be high impedance before the MCU drives TX data.
        if (err == ESP_OK) err = rmt_new_rx_channel(&rx, &rx_channel);
        if (err == ESP_OK) err = rmt_new_tx_channel(&tx, &tx_channel);
        if (err == ESP_OK) {
            const rmt_rx_event_callbacks_t callbacks = {.on_recv_done = rx_done};
            err = rmt_rx_register_event_callbacks(rx_channel, &callbacks, NULL);
        }
        const rmt_copy_encoder_config_t config = {};
        if (err == ESP_OK) err = rmt_new_copy_encoder(&config, &encoder);
        if (err != ESP_OK) {
            radio_ready = false; ESP_LOGE(TAG, "RMT initialization: %s", esp_err_to_name(err));
            if (encoder) { rmt_del_encoder(encoder); encoder = NULL; }
            if (tx_channel) { rmt_del_channel(tx_channel); tx_channel = NULL; }
            if (rx_channel) { rmt_del_channel(rx_channel); rx_channel = NULL; }
            free(rx_symbols); free(rx_snapshot); free(tx_symbols); rx_symbols = rx_snapshot = tx_symbols = NULL;
        }
    }
    const esp_timer_create_args_t timer_args = {
        .callback = scan_tick, .name = "rf_scan", .skip_unhandled_events = true,
    };
    esp_err_t timer_err = esp_timer_create(&timer_args, &scan_timer);
    if (timer_err != ESP_OK) return timer_err;
    if (xTaskCreate(worker, "rf_worker", 8192, NULL, 6, &worker_task) != pdPASS) {
        esp_timer_delete(scan_timer); scan_timer = NULL; return ESP_ERR_NO_MEM;
    }
    timer_err = esp_timer_start_periodic(scan_timer, 1000);
    if (timer_err != ESP_OK) {
        vTaskDelete(worker_task); worker_task = NULL;
        esp_timer_delete(scan_timer); scan_timer = NULL; return timer_err;
    }
    return ESP_OK;
}

esp_err_t rf_service_request(const rf_command_t *request)
{
    if (!commands || !request || request->type <= RF_CMD_NONE || request->type > RF_CMD_MONITOR) return ESP_ERR_INVALID_ARG;
    if (atomic_load(&ota_locked) && request->type != RF_CMD_STOP && request->type != RF_CMD_LIBRARY) return ESP_ERR_INVALID_STATE;
    if (request->type == RF_CMD_STOP) xQueueReset(commands);
    if (xQueueSend(commands, request, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (worker_task) xTaskNotifyGive(worker_task);
    return ESP_OK;
}
void rf_service_status(rf_status_t *output)
{
    xSemaphoreTake(status_lock, portMAX_DELAY); *output = status; xSemaphoreGive(status_lock);
}
esp_err_t rf_service_pause_for_ota(void)
{
    if (!paused) return ESP_ERR_INVALID_STATE;
    atomic_store(&ota_locked, true);
    xSemaphoreTake(paused, 0);
    const rf_command_t request = {.type = RF_CMD_STOP};
    esp_err_t err = rf_service_request(&request);
    if (err == ESP_OK && xSemaphoreTake(paused, pdMS_TO_TICKS(2000)) != pdTRUE) err = ESP_ERR_TIMEOUT;
    if (err != ESP_OK) atomic_store(&ota_locked, false);
    return err;
}
void rf_service_resume_after_ota(void) { atomic_store(&ota_locked, false); }

esp_err_t rf_service_sleep(bool sleeping)
{
    if (sleeping) {
        esp_err_t err = rf_service_pause_for_ota();
        if (err != ESP_OK) return err;
        err = esp_timer_stop(scan_timer);
        if (err == ESP_OK && radio_ready) err = bsp_radio_strobe(0x39, NULL); // SPWD
        if (err != ESP_OK) {
            if (!esp_timer_is_active(scan_timer)) esp_timer_start_periodic(scan_timer, 1000);
            rf_service_resume_after_ota();
        }
        return err;
    }
    esp_err_t err = radio_ready ? bsp_radio_strobe(0x36, NULL) : ESP_OK; // CS wakes CC1101, SIDLE.
    esp_err_t timer_err = esp_timer_start_periodic(scan_timer, 1000);
    rf_service_resume_after_ota();
    return err == ESP_OK ? timer_err : err;
}
