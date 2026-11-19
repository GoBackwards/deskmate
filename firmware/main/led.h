#pragma once

// On-board WS2812 RGB LED (GPIO48 on the Freenove FNK0085) and the BOOT button (GPIO0).
// A small task renders the current pattern at 30 Hz, so blinking keeps going while a
// model runs, and debounces the button.
enum class LedPattern {
    OFF,
    BOOT,     // white
    IDLE,     // dim white breathing: park your phone to start
    FOCUS,    // green
    GRACE,    // yellow, slow blink
    NUDGE,    // red, fast blink
    AWAY,     // blue
    BREAK,    // purple
    DONE,     // rainbow
    CAPTURE,  // cyan
    ERROR,    // red
};

void led_start(int led_gpio, int button_gpio);
void led_set(LedPattern p);
const char *led_name(LedPattern p);
// True once per press of the button.
bool led_take_button_press();
