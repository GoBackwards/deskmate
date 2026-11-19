#include "focus_fsm.h"

const char *state_name(FocusState s)
{
    switch (s) {
    case FocusState::IDLE: return "IDLE";
    case FocusState::FOCUS: return "FOCUS";
    case FocusState::GRACE: return "GRACE";
    case FocusState::NUDGE: return "NUDGE";
    case FocusState::AWAY: return "AWAY";
    case FocusState::BREAK: return "BREAK";
    case FocusState::DONE: return "DONE";
    }
    return "?";
}

const char *event_name(FocusEvent e)
{
    switch (e) {
    case FocusEvent::NONE: return "NONE";
    case FocusEvent::SESSION_START: return "SESSION_START";
    case FocusEvent::PHONE_OUT: return "PHONE_OUT";
    case FocusEvent::NUDGE: return "NUDGE";
    case FocusEvent::PHONE_BACK: return "PHONE_BACK";
    case FocusEvent::AWAY: return "AWAY";
    case FocusEvent::BACK: return "BACK";
    case FocusEvent::BREAK_START: return "BREAK_START";
    case FocusEvent::BREAK_END: return "BREAK_END";
    case FocusEvent::DONE: return "DONE";
    case FocusEvent::SESSION_END: return "SESSION_END";
    case FocusEvent::RESET: return "RESET";
    }
    return "?";
}

FocusFsm::FocusFsm(const FocusConfig &cfg) : cfg_(cfg) {}

void FocusFsm::enter(FocusState s, uint32_t now_ms)
{
    state_ = s;
    if (s == FocusState::GRACE) {
        grace_start_ms_ = now_ms;
    }
    if (s == FocusState::NUDGE) {
        last_nudge_ms_ = now_ms;
    }
}

uint32_t FocusFsm::grace_left_ms(uint32_t now_ms) const
{
    if (state_ != FocusState::GRACE) {
        return 0;
    }
    const uint32_t used = now_ms - grace_start_ms_;
    return used >= cfg_.grace_ms ? 0 : cfg_.grace_ms - used;
}

FocusEvent FocusFsm::step(uint32_t now_ms, bool phone_raw, bool person_raw)
{
    uint32_t dt = started_ ? now_ms - last_ms_ : 0;
    if (dt > cfg_.max_step_ms) {
        dt = cfg_.max_step_ms;
    }
    started_ = true;
    last_ms_ = now_ms;

    // Debounce the phone: flip only after N consecutive frames disagree.
    const bool phone_was_on = phone_on_;
    if (phone_raw != phone_on_) {
        phone_streak_++;
        if (phone_streak_ >= (phone_raw ? cfg_.phone_on_frames : cfg_.phone_off_frames)) {
            phone_on_ = phone_raw;
            phone_streak_ = 0;
        }
    } else {
        phone_streak_ = 0;
    }
    const bool phone_rose = phone_on_ && !phone_was_on;

    // Person: present at once when seen, absent after away_ms without a sighting.
    if (person_raw) {
        person_seen_ = true;
        person_last_ms_ = now_ms;
    }
    person_on_ = person_seen_ && (now_ms - person_last_ms_) < cfg_.away_ms;

    switch (state_) {
    case FocusState::IDLE:
        if (phone_on_ && person_on_) {
            focus_ms_ = 0;
            pickups_ = 0;
            enter(FocusState::FOCUS, now_ms);
            return FocusEvent::SESSION_START;
        }
        return FocusEvent::NONE;

    case FocusState::FOCUS:
        focus_ms_ += dt;
        if (focus_ms_ >= cfg_.session_ms) {
            focus_ms_ = cfg_.session_ms;
            enter(FocusState::DONE, now_ms);
            return FocusEvent::DONE;
        }
        if (!person_on_) {
            enter(FocusState::AWAY, now_ms);
            return FocusEvent::AWAY;
        }
        if (!phone_on_) {
            pickups_++;
            enter(FocusState::GRACE, now_ms);
            return FocusEvent::PHONE_OUT;
        }
        return FocusEvent::NONE;

    case FocusState::GRACE:
        if (phone_on_) {
            enter(FocusState::FOCUS, now_ms);
            return FocusEvent::PHONE_BACK;
        }
        if (!person_on_) {
            enter(FocusState::AWAY, now_ms);
            return FocusEvent::AWAY;
        }
        if (now_ms - grace_start_ms_ >= cfg_.grace_ms) {
            enter(FocusState::NUDGE, now_ms);
            return FocusEvent::NUDGE;
        }
        return FocusEvent::NONE;

    case FocusState::NUDGE:
        if (phone_on_) {
            enter(FocusState::FOCUS, now_ms);
            return FocusEvent::PHONE_BACK;
        }
        if (!person_on_) {
            enter(FocusState::AWAY, now_ms);
            return FocusEvent::AWAY;
        }
        if (now_ms - last_nudge_ms_ >= cfg_.renudge_ms) {
            last_nudge_ms_ = now_ms;
            return FocusEvent::NUDGE;
        }
        return FocusEvent::NONE;

    case FocusState::AWAY:
        if (person_on_) {
            enter(phone_on_ ? FocusState::FOCUS : FocusState::GRACE, now_ms);
            return FocusEvent::BACK;
        }
        return FocusEvent::NONE;

    case FocusState::BREAK:
        if (!phone_on_) {
            break_phone_left_ = true;
        } else if (phone_rose && break_phone_left_) {
            enter(FocusState::FOCUS, now_ms);
            return FocusEvent::BREAK_END;
        }
        return FocusEvent::NONE;

    case FocusState::DONE:
        if (!phone_on_) {
            enter(FocusState::IDLE, now_ms);
            return FocusEvent::SESSION_END;
        }
        return FocusEvent::NONE;
    }
    return FocusEvent::NONE;
}

FocusEvent FocusFsm::toggle_break(uint32_t now_ms)
{
    switch (state_) {
    case FocusState::FOCUS:
    case FocusState::GRACE:
    case FocusState::NUDGE:
    case FocusState::AWAY:
        break_phone_left_ = !phone_on_;
        enter(FocusState::BREAK, now_ms);
        return FocusEvent::BREAK_START;
    case FocusState::BREAK:
        enter(phone_on_ ? FocusState::FOCUS : FocusState::GRACE, now_ms);
        return FocusEvent::BREAK_END;
    default:
        return FocusEvent::NONE;
    }
}

FocusEvent FocusFsm::reset()
{
    state_ = FocusState::IDLE;
    focus_ms_ = 0;
    pickups_ = 0;
    return FocusEvent::RESET;
}
