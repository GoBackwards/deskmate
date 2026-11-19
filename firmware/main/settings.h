#pragma once
#include <cstdint>

// Demo settings kept in NVS, so the pad position and timings survive a reset.
struct Settings {
    uint32_t version;
    int16_t roi[4];        // phone pad region x1, y1, x2, y2 in the 320x240 frame
    uint32_t session_s;    // focus time for one session
    uint32_t grace_s;      // phone off the pad this long -> nudge
    uint32_t away_s;       // nobody seen this long -> away
    uint32_t renudge_s;    // repeat the nudge
    float phone_thr;       // p(phone on pad) threshold
    float person_thr;      // person box score threshold
    float person_min_area; // smallest person box, as a fraction of the frame (ignores people far away)
    uint8_t person_enabled;
};

void settings_defaults(Settings &s);
bool settings_load(Settings &s);   // false (and defaults) if nothing valid is stored
bool settings_save(const Settings &s);
