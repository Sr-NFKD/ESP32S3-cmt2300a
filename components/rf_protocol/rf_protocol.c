#include "rf_protocol.h"
#include <string.h>
#include <stdio.h>

static bool near(unsigned actual, unsigned expected)
{
    return expected && actual * 100 >= expected * 65 && actual * 100 <= expected * 135;
}

bool rf_frame_encode(rf_frame_t *frame, uint32_t code, uint8_t bits, uint16_t pulse)
{
    if (!frame || bits < 8 || bits > 32 || pulse < 100 || pulse > 1000 ||
        (bits < 32 && code >= (1UL << bits))) return false;
    frame->code = code; frame->bits = bits; frame->pulse_us = pulse;
    frame->first_level = 1; frame->count = 0;
    frame->tail_clipped = false;
    for (int bit = bits - 1; bit >= 0; bit--) {
        bool one = (code >> bit) & 1;
        frame->duration[frame->count++] = pulse * (one ? 3 : 1);
        frame->duration[frame->count++] = pulse * (one ? 1 : 3);
    }
    frame->duration[frame->count++] = pulse;
    frame->duration[frame->count++] = pulse * 31;
    return true;
}

static void extract_repeated_raw(rf_frame_t *frame)
{
    // Only bounded, matching frames between long gaps are learnable from a
    // continuous stream. Never treat the cut ends of a DMA block as a frame.
    unsigned gap = 0;
    for (unsigned i = 1; i + 1 < frame->count; i++)
        if (frame->duration[i] > gap) gap = frame->duration[i];
    if (gap < 4000) return;
    unsigned previous = RF_MAX_PULSES, reference = 0, length = 0;
    for (unsigned i = 1; i + 1 < frame->count; i++) {
        if (frame->duration[i] * 100U < gap * 80U) continue;
        if (previous != RF_MAX_PULSES) {
            unsigned start = previous + 1, count = i - previous;
            bool same = count >= 16 && count == length && !((start ^ reference) & 1);
            for (unsigned n = 0; same && n < count; n++)
                same = frame->duration[start+n] >= 60 &&
                    frame->duration[reference+n] >= 60 &&
                    near(frame->duration[start+n], frame->duration[reference+n]);
            if (same) {
                frame->first_level ^= reference & 1;
                memmove(frame->duration, frame->duration + reference, length * sizeof(uint16_t));
                frame->count = length;
                frame->repeats = 2;
                frame->tail_clipped = false;
                return;
            }
            reference = start;
            length = count;
        }
        previous = i;
    }
}

bool rf_raw40_decode(const uint16_t *d, size_t count, uint8_t first_level, uint64_t *code)
{
    if (!d || !code || count != 82 || first_level != 1 ||
        d[0] < 4000 || d[0] > 5600 || d[1] < 1100 || d[1] > 1900 ||
        d[81] < 3000 || d[81] > 32767) return false;
    uint64_t value = 0;
    // Measured 1:2 OOK timing; Dooya framing corroborated by Flipper's decoder.
    // Last low includes the gap, so its bit is determined by the high duration.
    for (unsigned bit = 0; bit < 40; bit++) {
        unsigned h = d[2+bit*2], l = d[3+bit*2];
        bool zero = h >= 250 && h <= 480 && (bit == 39 || (l >= 560 && l <= 900));
        bool one = h >= 560 && h <= 900 && (bit == 39 || (l >= 250 && l <= 480));
        if (!zero && !one) return false;
        value = (value << 1) | one;
    }
    *code = value;
    return true;
}

bool rf_frame_analyze(rf_frame_t *frame)
{
    if (!frame || frame->count < 16 || frame->count > RF_MAX_PULSES || frame->first_level > 1) return false;
    // Remove only an internal narrow opposite-level spike. Its two neighbours
    // are the same level; merge all three durations so time and polarity survive.
    for (unsigned i = 1; i + 1 < frame->count;) {
        unsigned sum = frame->duration[i-1] + frame->duration[i] + frame->duration[i+1];
        if (frame->duration[i] < 60 && frame->duration[i-1] >= 80 &&
            frame->duration[i+1] >= 80 && sum <= 32767) {
            frame->duration[i-1] = sum;
            memmove(&frame->duration[i], &frame->duration[i+2],
                    (frame->count-i-2) * sizeof(frame->duration[0]));
            frame->count -= 2;
            if (i > 1) i--;
        } else i++;
    }
    frame->bits = 0; frame->repeats = 0; frame->pulse_us = 0; frame->code = 0;
    frame->raw_code[0] = 0;
    unsigned raw_start = 0, raw_repeats = 0;
    uint64_t raw_code = 0;
    for (unsigned i = 0; i + 82 <= frame->count; i++) {
        uint64_t value;
        if (!rf_raw40_decode(frame->duration+i, 82, frame->first_level ^ (i & 1), &value)) continue;
        if (!raw_repeats) { raw_start = i; raw_code = value; raw_repeats = 1; }
        else if (raw_code == value) raw_repeats++;
        i += 81;
    }
    if (raw_repeats) {
        memmove(frame->duration, frame->duration+raw_start, 82 * sizeof(uint16_t));
        frame->first_level = 1; frame->count = 82; frame->tail_clipped = false;
        frame->repeats = raw_repeats;
        snprintf(frame->raw_code, sizeof(frame->raw_code), "%010llX", (unsigned long long)raw_code);
        return true;
    }
    unsigned start = frame->first_level ? 0 : 1;
    uint32_t code = 0;
    unsigned bits = 0, sum = 0;
    bool invalid = false;
    for (unsigned i = start; i + 1 < frame->count; i += 2) {
        unsigned high = frame->duration[i], low = frame->duration[i + 1];
        if (high < 80 || low < 80) { invalid = true; continue; }
        // A long low period closes a frame. The hardware may truncate its final gap.
        if (low > high * 10 && high >= 100 && high <= 1000) {
            unsigned pulse = bits ? sum / (bits * 4) : 0;
            unsigned sync = pulse * 31;
            bool valid_sync = sync && low * 100 >= sync * 80 && low * 100 <= sync * 120 &&
                !(frame->tail_clipped && i + 2 == frame->count);
            if (!invalid && bits >= 8 && bits <= 32 && near(high, pulse) && valid_sync) {
                if (!frame->bits) {
                    frame->code = code; frame->bits = bits; frame->pulse_us = pulse; frame->repeats = 1;
                } else if (frame->bits == bits && frame->code == code) {
                    if (frame->repeats < 255) frame->repeats++;
                } else if (frame->repeats < 2 && bits > frame->bits) {
                    // Prefer a full later frame over a capture that began mid-frame.
                    frame->code = code; frame->bits = bits; frame->pulse_us = pulse; frame->repeats = 1;
                }
            }
            bits = sum = code = 0;
            invalid = false;
            continue;
        }
        unsigned unit = (high + low) / 4;
        bool zero = near(high, unit) && near(low, unit * 3);
        bool one = near(high, unit * 3) && near(low, unit);
        if (invalid) continue;
        if ((!zero && !one) || unit < 100 || unit > 1000 || bits >= 32) { invalid = true; continue; }
        code = (code << 1) | one; bits++; sum += high + low;
    }
    if (frame->bits) {
        uint8_t repeats = frame->repeats;
        rf_frame_encode(frame, frame->code, frame->bits, frame->pulse_us);
        frame->repeats = repeats;
    } else extract_repeated_raw(frame);
    return true;
}

bool rf_frame_same(const rf_frame_t *a, const rf_frame_t *b)
{
    if (!a || !b || a->frequency_hz != b->frequency_hz) return false;
    if (a->bits || b->bits) return a->bits == b->bits && a->code == b->code;
    if (a->raw_code[0] || b->raw_code[0]) return !strcmp(a->raw_code, b->raw_code);
    if (a->count != b->count || a->first_level != b->first_level) return false;
    for (unsigned i = 0; i < a->count; i++) if (!near(a->duration[i], b->duration[i])) return false;
    return true;
}

bool rf_code_parse(const char *text, uint8_t bits, uint32_t *code)
{
    if (!text || !*text || !code || bits < 8 || bits > 32 || strlen(text) > (bits + 3U) / 4U) return false;
    uint32_t value = 0;
    for (const char *p = text; *p; p++) {
        unsigned digit = *p >= '0' && *p <= '9' ? *p - '0' :
            *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : 16;
        if (digit > 15) return false;
        value = (value << 4) | digit;
    }
    if (bits < 32 && value >= (1UL << bits)) return false;
    *code = value;
    return true;
}
