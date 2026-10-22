#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RF_MAX_PULSES 1024
#define RF_LIBRARY_LIMIT 32
typedef struct {
    uint32_t frequency_hz;
    uint32_t code;
    uint16_t pulse_us, count;
    uint8_t first_level, bits, repeats;
    bool tail_clipped;
    char raw_code[11]; // Decoded 40-bit RAW key; waveform remains authoritative for TX.
    uint16_t duration[RF_MAX_PULSES];
} rf_frame_t;

// Common short/long OOK PWM (1:3, sync 1:31), independently implemented.
// Unknown protocols keep original level durations and bits=0.
bool rf_frame_analyze(rf_frame_t *frame);
bool rf_raw40_decode(const uint16_t *durations, size_t count, uint8_t first_level, uint64_t *code);
bool rf_frame_encode(rf_frame_t *frame, uint32_t code, uint8_t bits, uint16_t pulse_us);
bool rf_frame_same(const rf_frame_t *a, const rf_frame_t *b);
bool rf_code_parse(const char *text, uint8_t bits, uint32_t *code);

typedef enum { RF_CMD_NONE, RF_CMD_RECEIVE, RF_CMD_STOP, RF_CMD_SAVE,
               RF_CMD_LIBRARY, RF_CMD_SEND, RF_CMD_DELETE, RF_CMD_MONITOR } rf_command_type_t;
typedef struct { rf_command_type_t type; uint32_t frequency_hz, id; bool keep_result; } rf_command_t;
typedef enum { RF_IDLE, RF_LISTENING, RF_CAPTURED, RF_SAVED, RF_SENDING,
               RF_SENT, RF_MONITORING, RF_FAILURE } rf_state_t;
typedef struct { uint32_t id, frequency_hz; char code[17]; bool raw; } rf_library_item_t;
typedef struct {
    rf_state_t state;
    int64_t last_signal_us; // Monotonic time of the latest RF signal; 0 before any signal.
    uint32_t frequency_hz, saved_id, revision;
    int16_t rssi_dbm;
    uint16_t pulse_count;
    uint8_t occupancy, bits, library_count;
    bool raw;
    char code[17], error[40];
    rf_library_item_t library[RF_LIBRARY_LIMIT];
} rf_status_t;
