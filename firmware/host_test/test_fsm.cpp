// Host test for main/focus_fsm.cpp. Build and run (from E:\esp\deskmate\firmware):
//   uv run --no-project --python 3.12 --with ziglang python -m ziglang c++ -std=c++17 -O1 -Imain
//       host_test/test_fsm.cpp main/focus_fsm.cpp -o build_host/test_fsm.exe && build_host/test_fsm.exe
#include <cstdio>
#include <cstdlib>

#include "focus_fsm.h"

static int g_fail = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

// Frames 1 s apart, like the board's ~1 Hz loop.
struct Sim {
    FocusFsm fsm;
    uint32_t t = 0;
    explicit Sim(const FocusConfig &c) : fsm(c) {}
    FocusEvent frame(bool phone, bool person, uint32_t dt = 1000)
    {
        t += dt;
        return fsm.step(t, phone, person);
    }
    // Run n frames, return the last non-NONE event (or NONE).
    FocusEvent run(int n, bool phone, bool person)
    {
        FocusEvent last = FocusEvent::NONE;
        for (int i = 0; i < n; i++) {
            FocusEvent e = frame(phone, person);
            if (e != FocusEvent::NONE) last = e;
        }
        return last;
    }
};

static FocusConfig demo_cfg()
{
    FocusConfig c;
    c.session_ms = 60000;
    c.grace_ms = 5000;
    c.renudge_ms = 10000;
    c.away_ms = 3000;
    return c;
}

static void test_start_needs_phone_and_person()
{
    Sim s(demo_cfg());
    CHECK(s.frame(true, false) == FocusEvent::NONE);           // phone, 1st frame, nobody yet
    CHECK(s.fsm.state() == FocusState::IDLE);
    CHECK(s.frame(true, false) == FocusEvent::NONE);           // phone debounced, still nobody
    CHECK(s.fsm.state() == FocusState::IDLE);
    CHECK(s.frame(true, true) == FocusEvent::SESSION_START);
    CHECK(s.fsm.state() == FocusState::FOCUS);
    CHECK(s.fsm.focus_ms() == 0);
    // A person stays "present" for away_ms after the last sighting.
    Sim p(demo_cfg());
    p.frame(false, true);
    p.frame(true, false);
    CHECK(p.frame(true, false) == FocusEvent::SESSION_START);  // seen 2 s ago < 3 s
}

static void test_glitch_does_not_flip_phone()
{
    Sim s(demo_cfg());
    s.run(3, true, true);
    CHECK(s.fsm.state() == FocusState::FOCUS);
    CHECK(s.frame(false, true) == FocusEvent::NONE);  // one bad frame
    CHECK(s.fsm.state() == FocusState::FOCUS);
    CHECK(s.frame(true, true) == FocusEvent::NONE);
    CHECK(s.fsm.state() == FocusState::FOCUS);
    CHECK(s.fsm.pickups() == 0);
}

static void test_pickup_grace_nudge_back()
{
    Sim s(demo_cfg());
    s.run(3, true, true);  // start (2nd frame) + 1 s of focus
    const uint32_t start = s.fsm.focus_ms();
    CHECK(start == 1000);
    s.run(10, true, true); // 10 s of focus
    CHECK(s.fsm.focus_ms() == start + 10000);
    CHECK(s.frame(false, true) == FocusEvent::NONE);
    CHECK(s.frame(false, true) == FocusEvent::PHONE_OUT);  // after 2 off frames
    CHECK(s.fsm.state() == FocusState::GRACE);
    CHECK(s.fsm.pickups() == 1);
    // The frame that saw the 2nd "off" still counted as focus (it was FOCUS then).
    const uint32_t at_out = s.fsm.focus_ms();
    CHECK(s.run(4, false, true) == FocusEvent::NONE);       // 4 s < 5 s grace
    CHECK(s.fsm.state() == FocusState::GRACE);
    CHECK(s.frame(false, true) == FocusEvent::NUDGE);       // 5 s
    CHECK(s.fsm.state() == FocusState::NUDGE);
    CHECK(s.run(9, false, true) == FocusEvent::NONE);
    CHECK(s.frame(false, true) == FocusEvent::NUDGE);       // repeat after 10 s
    CHECK(s.fsm.focus_ms() == at_out);                      // timer paused off the pad
    CHECK(s.frame(true, true) == FocusEvent::NONE);
    CHECK(s.frame(true, true) == FocusEvent::PHONE_BACK);
    CHECK(s.fsm.state() == FocusState::FOCUS);
}

static void test_away_pauses_and_back_resumes()
{
    Sim s(demo_cfg());
    s.run(3, true, true);
    s.run(5, true, true);
    // Person gone: away after 3 s without a sighting.
    CHECK(s.frame(true, false) == FocusEvent::NONE);
    CHECK(s.frame(true, false) == FocusEvent::NONE);
    CHECK(s.frame(true, false) == FocusEvent::AWAY);
    const uint32_t paused = s.fsm.focus_ms();
    s.run(20, true, false);
    CHECK(s.fsm.state() == FocusState::AWAY);
    CHECK(s.fsm.focus_ms() == paused);
    CHECK(s.frame(true, true) == FocusEvent::BACK);
    CHECK(s.fsm.state() == FocusState::FOCUS);
    // Came back without the phone: grace, not focus.
    s.run(3, true, false);
    CHECK(s.fsm.state() == FocusState::AWAY);
    s.run(2, false, false);
    CHECK(s.frame(false, true) == FocusEvent::BACK);
    CHECK(s.fsm.state() == FocusState::GRACE);
}

static void test_break()
{
    Sim s(demo_cfg());
    s.run(3, true, true);
    s.run(5, true, true);
    CHECK(s.fsm.toggle_break(s.t) == FocusEvent::BREAK_START);
    CHECK(s.fsm.state() == FocusState::BREAK);
    const uint32_t paused = s.fsm.focus_ms();
    s.run(10, false, true);   // takes the phone: no nudge during a break
    CHECK(s.fsm.state() == FocusState::BREAK);
    CHECK(s.fsm.focus_ms() == paused);
    CHECK(s.frame(true, true) == FocusEvent::NONE);
    CHECK(s.frame(true, true) == FocusEvent::BREAK_END);  // phone back ends the break
    CHECK(s.fsm.state() == FocusState::FOCUS);
    // Button again during a break with the phone still parked -> back to focus.
    CHECK(s.fsm.toggle_break(s.t) == FocusEvent::BREAK_START);
    s.run(3, true, true);
    CHECK(s.fsm.state() == FocusState::BREAK);  // phone never left: stays in break
    CHECK(s.fsm.toggle_break(s.t) == FocusEvent::BREAK_END);
    CHECK(s.fsm.state() == FocusState::FOCUS);
    // Break from IDLE does nothing.
    s.fsm.reset();
    CHECK(s.fsm.toggle_break(s.t) == FocusEvent::NONE);
}

static void test_done_and_session_end()
{
    Sim s(demo_cfg());
    s.run(3, true, true);
    FocusEvent e = s.run(60, true, true);
    CHECK(e == FocusEvent::DONE);
    CHECK(s.fsm.state() == FocusState::DONE);
    CHECK(s.fsm.focus_ms() == 60000);
    s.frame(false, true);
    CHECK(s.frame(false, true) == FocusEvent::SESSION_END);
    CHECK(s.fsm.state() == FocusState::IDLE);
}

static void test_long_stall_is_capped()
{
    Sim s(demo_cfg());
    s.run(3, true, true);
    const uint32_t before = s.fsm.focus_ms();
    s.frame(true, true, 60000);  // the loop stalled for a minute
    CHECK(s.fsm.focus_ms() - before == 5000);
}

int main()
{
    test_start_needs_phone_and_person();
    test_glitch_does_not_flip_phone();
    test_pickup_grace_nudge_back();
    test_away_pauses_and_back_resumes();
    test_break();
    test_done_and_session_end();
    test_long_stall_is_capped();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all FSM tests passed\n");
    return 0;
}
