#include "hid_feedback.h"
#include "bt_host.h"
#include "bluebridge_state.h"
#include <string.h>

#define BB_EFFECT_COUNT 8

typedef struct {
    bool allocated;
    bool playing;
    uint8_t gain;
    uint8_t loops;
    uint16_t duration;
    uint16_t delay;
    uint16_t magnitude;
    int16_t offset;
    uint16_t period;
    uint16_t attack_level;
    uint16_t fade_level;
    uint16_t attack_time;
    uint16_t fade_time;
    uint32_t started;
} bb_effect_t;

typedef struct {
    bb_effect_t effects[BB_EFFECT_COUNT];
    uint8_t device_gain;
    bool enabled;
    bool paused;
    uint32_t paused_at;
    uint32_t clock_now;
    uint32_t last_sent_at;
    uint8_t last_strength;
    uint8_t last_left;    // big motor sent last
    uint8_t last_right;   // small motor sent last
    uint8_t load_index;
    uint8_t load_status;
} bb_feedback_context_t;
static bb_feedback_context_t feedback_slots[BB_MAX_SLOTS] = {
{.device_gain=255,.enabled=true,.load_status=3},
{.device_gain=255,.enabled=true,.load_status=3},
{.device_gain=255,.enabled=true,.load_status=3},
{.device_gain=255,.enabled=true,.load_status=3}
};
static uint8_t feedback_slot;
void bb_feedback_select(uint8_t slot) { if (slot < BB_MAX_SLOTS) feedback_slot = slot; }
#define effects (feedback_slots[feedback_slot].effects)
#define device_gain (feedback_slots[feedback_slot].device_gain)
#define enabled (feedback_slots[feedback_slot].enabled)
#define paused (feedback_slots[feedback_slot].paused)
#define paused_at (feedback_slots[feedback_slot].paused_at)
#define clock_now (feedback_slots[feedback_slot].clock_now)
#define last_sent_at (feedback_slots[feedback_slot].last_sent_at)
#define last_strength (feedback_slots[feedback_slot].last_strength)
#define last_left (feedback_slots[feedback_slot].last_left)
#define last_right (feedback_slots[feedback_slot].last_right)
#define load_index (feedback_slots[feedback_slot].load_index)
#define load_status (feedback_slots[feedback_slot].load_status)

// ---- Recover both motors from Linux FF_RUMBLE ----
// Linux has no rumble in hid-pidff, so ff-core turns an FF_RUMBLE effect into one sine of
// magnitude strong/3 + weak/6 (drivers/input/ff-core.c compat_effect, period 50 ms). MiSTer
// sends strong = big*257 and weak = small*257 (Main_MiSTer input.cpp set_rumble). With the
// small motor either off or full (PSX DualShock, N64 Rumble Pak), the two cases never produce
// the same magnitude, so both motors can be recovered exactly. Returns false otherwise.
static int find_big(int32_t part) {
    if (part < 0 || part > 21845) return -1;
    // part = (big * 257) / 3 rounded down; big is about part * 3 / 257
    int32_t guess = (part * 3) / 257;
    for (int32_t b = guess - 1; b <= guess + 1; ++b) {
        if (b >= 0 && b <= 255 && (b * 257) / 3 == part) return (int)b;
    }
    return -1;
}

static bool bb_rumble_decode(uint16_t magnitude, uint8_t* big, uint8_t* small) {
    const int32_t small_full = (255 * 257) / 6;   // weak 0xffff / 6 = 10922
    int b = find_big((int32_t)magnitude);
    if (b >= 0) { *big = (uint8_t)b; *small = 0; return true; }
    b = find_big((int32_t)magnitude - small_full);
    if (b >= 0) { *big = (uint8_t)b; *small = 255; return true; }
    return false;
}

static uint16_t read16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write16(uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static unsigned available(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < BB_EFFECT_COUNT; ++i) if (!effects[i].allocated) ++count;
    return count * 64u;
}

void bb_feedback_stop(void) {
    for (unsigned i = 0; i < BB_EFFECT_COUNT; ++i) effects[i].playing = false;
    if (last_strength) bb_bt_host_set_slot_rumble(feedback_slot, 0, 0);
    last_strength = 0;
    last_left = last_right = 0;
    last_sent_at = clock_now;
}

void bb_feedback_reset(void) {
    memset(effects, 0, sizeof(effects));
    device_gain = 255;
    enabled = true;
    paused = false;
    load_index = 0;
    load_status = 3;
    if (last_strength) bb_bt_host_set_slot_rumble(feedback_slot, 0, 0);
    last_strength = 0;
    last_left = last_right = 0;
    last_sent_at = clock_now;
}

void bb_feedback_set(uint8_t report_id, bool feature, const uint8_t* data, uint16_t length) {
    if (!data) return;
    if (feature) {
        if (report_id != 9 || length != 1) return;
        load_index = 0;
        load_status = data[0] == 1 ? 2 : 3;
        if (data[0] != 1) return;
        for (unsigned i = 0; i < BB_EFFECT_COUNT; ++i) {
            if (effects[i].allocated) continue;
            memset(&effects[i], 0, sizeof(effects[i]));
            effects[i].allocated = true;
            effects[i].gain = 255;
            load_index = (uint8_t)(i + 1);
            load_status = 1;
            break;
        }
        return;
    }
    if (report_id == 6 && length == 1) {
        device_gain = data[0];
        return;
    }
    if (report_id == 7 && length == 1) {
        switch (data[0]) {
            case 1: enabled = true; break;
            case 2: enabled = false; break;
            case 3: bb_feedback_stop(); break;
            case 4: bb_feedback_reset(); break;
            case 5:
                if (!paused) { paused = true; paused_at = clock_now; }
                break;
            case 6:
                if (paused) {
                    uint32_t elapsed = clock_now - paused_at;
                    for (unsigned i = 0; i < BB_EFFECT_COUNT; ++i) if (effects[i].playing) effects[i].started += elapsed;
                    paused = false;
                }
                break;
            default: break;
        }
        return;
    }
    if (!length || data[0] < 1 || data[0] > BB_EFFECT_COUNT) return;
    bb_effect_t* effect = &effects[data[0] - 1];
    if (!effect->allocated) return;
    if (report_id == 2 && length == 14 && data[1] == 1) {
        effect->duration = read16(data + 2);
        effect->gain = data[4];
        effect->delay = read16(data + 12);
    } else if (report_id == 3 && length == 9) {
        uint16_t magnitude = read16(data + 1);
        effect->magnitude = magnitude > 32767 ? 32767 : magnitude;
        effect->offset = (int16_t)read16(data + 3);
        effect->period = read16(data + 7);
    } else if (report_id == 4 && length == 9) {
        effect->attack_level = read16(data + 1);
        effect->fade_level = read16(data + 3);
        effect->attack_time = read16(data + 5);
        effect->fade_time = read16(data + 7);
        if (effect->attack_level > 32767) effect->attack_level = 32767;
        if (effect->fade_level > 32767) effect->fade_level = 32767;
    } else if (report_id == 5 && length == 3) {
        if (data[1] == 3 || !data[2]) effect->playing = false;
        else if (data[1] == 1 || data[1] == 2) {
            if (data[1] == 2) for (unsigned i = 0; i < BB_EFFECT_COUNT; ++i) effects[i].playing = false;
            effect->playing = true;
            effect->loops = data[2];
            effect->started = paused ? paused_at : clock_now;
        }
    } else if (report_id == 8 && length == 1) {
        memset(effect, 0, sizeof(*effect));
    }
}

uint16_t bb_feedback_get(uint8_t report_id, bool feature, uint8_t* data, uint16_t length) {
    if (!feature || !data) return 0;
    uint8_t report[4] = {0};
    uint16_t size;
    if (report_id == 10) {
        report[0] = load_index;
        report[1] = load_status;
        write16(report + 2, (uint16_t)available());
        size = 4;
    } else if (report_id == 11) {
        write16(report, BB_EFFECT_COUNT * 64u);
        report[2] = BB_EFFECT_COUNT;
        report[3] = 1;
        size = 4;
    } else return 0;
    if (size > length) size = length;
    memcpy(data, report, size);
    return size;
}

void bb_feedback_poll(uint32_t now) {
    clock_now = now;
    uint32_t peak = 0;
    unsigned playing = 0;
    const bb_effect_t* only = NULL;
    if (enabled && !paused) {
        for (unsigned i = 0; i < BB_EFFECT_COUNT; ++i) {
            bb_effect_t* effect = &effects[i];
            if (!effect->allocated || !effect->playing) continue;
            uint32_t elapsed = now - effect->started;
            if (elapsed < effect->delay) continue;
            elapsed -= effect->delay;
            bool infinite = effect->duration == 65535u;
            if (!infinite) {
                uint32_t total = (uint32_t)effect->duration * effect->loops;
                if (!effect->duration || (effect->loops != 255 && elapsed >= total)) {
                    effect->playing = false;
                    continue;
                }
                elapsed %= effect->duration;
            }
            playing++;
            only = effect;
            int32_t offset = effect->offset;
            if (offset < 0) offset = -offset;
            int32_t amplitude = effect->magnitude + offset;
            if (amplitude > 32767) amplitude = 32767;
            if (effect->attack_time && elapsed < effect->attack_time) {
                amplitude = effect->attack_level + (amplitude - effect->attack_level) * (int32_t)elapsed / effect->attack_time;
            }
            if (!infinite && effect->fade_time && effect->duration - elapsed < effect->fade_time) {
                uint32_t remaining = effect->duration - elapsed;
                amplitude = effect->fade_level + (amplitude - effect->fade_level) * (int32_t)remaining / effect->fade_time;
            }
            uint32_t strength = (uint32_t)amplitude * effect->gain * device_gain / (32767u * 255u);
            if (strength > peak) peak = strength;
        }
    }
    uint8_t strength = (uint8_t)peak;
    // A single plain rumble from Linux (FF_RUMBLE converted to a 50 ms sine at full gain) carries
    // both motors exactly; recover them. Otherwise the motors stay unknown (0/0) and controllers
    // with separate motors fall back to the merged strength.
    uint8_t left = 0, right = 0;
    uint8_t big, small;
    if (playing == 1 && only->period == 50 && only->offset == 0 && !only->attack_time && !only->fade_time &&
        only->gain == 255 && device_gain == 255 && bb_rumble_decode(only->magnitude, &big, &small)) {
        left = big;
        right = small;
    }
    bool changed = strength != last_strength || left != last_left || right != last_right;
    if (changed || (strength && (uint32_t)(now - last_sent_at) >= 250u)) {
        bb_bt_host_set_slot_rumble_motors(feedback_slot, strength, left, right);
        last_strength = strength;
        last_left = left;
        last_right = right;
        last_sent_at = now;
    }
}

