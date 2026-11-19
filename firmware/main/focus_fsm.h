// Deskmate focus-session state machine. Plain C++ (no ESP-IDF), so it runs in host tests.
//
// Park the phone on the pad while you sit at the desk -> the focus timer runs.
// Phone leaves the pad -> grace period, then a nudge (repeated) until it is back.
// Nobody at the desk -> timer paused (AWAY). Break button -> timer paused, no nudges.
// Timer reaches the session length -> DONE; taking the phone then ends the session.
#pragma once
#include <cstdint>

enum class FocusState : uint8_t { IDLE, FOCUS, GRACE, NUDGE, AWAY, BREAK, DONE };

enum class FocusEvent : uint8_t {
    NONE,
    SESSION_START,
    PHONE_OUT,
    NUDGE,
    PHONE_BACK,
    AWAY,
    BACK,
    BREAK_START,
    BREAK_END,
    DONE,
    SESSION_END,
    RESET,
};

const char *state_name(FocusState s);
const char *event_name(FocusEvent e);

struct FocusConfig {
    uint32_t session_ms = 25u * 60u * 1000u;  // focus time needed to finish a session
    uint32_t grace_ms = 10u * 1000u;          // phone off the pad this long -> nudge
    uint32_t renudge_ms = 30u * 1000u;        // repeat the nudge while it stays off
    uint32_t away_ms = 5u * 1000u;            // no person seen this long -> AWAY
    int phone_on_frames = 2;                  // consecutive frames to accept "on the pad"
    int phone_off_frames = 2;                 // consecutive frames to accept "off the pad"
    uint32_t max_step_ms = 5000;              // longest time one step may add to the timer
};

class FocusFsm {
public:
    explicit FocusFsm(const FocusConfig &cfg = FocusConfig());

    // One camera frame at time now_ms: the classifier's raw "phone on pad" decision and the
    // detector's raw "person at the desk" decision. Returns the event of this step, if any.
    FocusEvent step(uint32_t now_ms, bool phone_raw, bool person_raw);
    FocusEvent toggle_break(uint32_t now_ms);  // break button
    FocusEvent reset();                        // back to IDLE, timer cleared

    FocusState state() const { return state_; }
    uint32_t focus_ms() const { return focus_ms_; }
    int pickups() const { return pickups_; }
    bool phone_on() const { return phone_on_; }
    bool person_on() const { return person_on_; }
    uint32_t grace_left_ms(uint32_t now_ms) const;
    FocusConfig &config() { return cfg_; }

private:
    void enter(FocusState s, uint32_t now_ms);

    FocusConfig cfg_;
    FocusState state_ = FocusState::IDLE;
    bool started_ = false;       // a step has run (last_ms_ valid)
    uint32_t last_ms_ = 0;
    uint32_t focus_ms_ = 0;
    int pickups_ = 0;
    bool phone_on_ = false;      // debounced
    int phone_streak_ = 0;       // frames disagreeing with phone_on_
    bool person_on_ = false;     // debounced
    bool person_seen_ = false;
    uint32_t person_last_ms_ = 0;
    uint32_t grace_start_ms_ = 0;
    uint32_t last_nudge_ms_ = 0;
    bool break_phone_left_ = false;  // the phone left the pad during this break
};
