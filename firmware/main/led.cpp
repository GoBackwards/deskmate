#include "led.h"

#include <atomic>
#include <cmath>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led";
static led_strip_handle_t s_strip = nullptr;
static std::atomic<int> s_pattern{(int)LedPattern::BOOT};
static std::atomic<bool> s_pressed{false};
static int s_button_gpio = -1;

static constexpr int kMax = 60;  // full brightness is blinding; 60/255 is plenty indoors

const char *led_name(LedPattern p)
{
    switch (p) {
    case LedPattern::OFF: return "off";
    case LedPattern::BOOT: return "white";
    case LedPattern::IDLE: return "white-breathe";
    case LedPattern::FOCUS: return "green";
    case LedPattern::GRACE: return "yellow-blink";
    case LedPattern::NUDGE: return "red-fast-blink";
    case LedPattern::AWAY: return "blue";
    case LedPattern::BREAK: return "purple";
    case LedPattern::DONE: return "rainbow";
    case LedPattern::CAPTURE: return "cyan";
    case LedPattern::ERROR: return "red";
    }
    return "?";
}

static void hsv(float h, int v, int &r, int &g, int &b)
{
    const float x = 1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f);
    float rf = 0, gf = 0, bf = 0;
    if (h < 60) { rf = 1; gf = x; }
    else if (h < 120) { rf = x; gf = 1; }
    else if (h < 180) { gf = 1; bf = x; }
    else if (h < 240) { gf = x; bf = 1; }
    else if (h < 300) { rf = x; bf = 1; }
    else { rf = 1; bf = x; }
    r = (int)(rf * v);
    g = (int)(gf * v);
    b = (int)(bf * v);
}

static void render(LedPattern p, int64_t t_ms)
{
    int r = 0, g = 0, b = 0;
    const bool slow = (t_ms / 500) % 2 == 0;   // 1 Hz blink
    const bool fast = (t_ms / 125) % 2 == 0;   // 4 Hz blink
    switch (p) {
    case LedPattern::OFF: break;
    case LedPattern::BOOT: r = g = b = kMax / 2; break;
    case LedPattern::IDLE: {
        const float k = 0.5f - 0.5f * cosf((float)(t_ms % 3000) / 3000.0f * 2.0f * (float)M_PI);
        r = g = b = 2 + (int)(k * kMax / 3);
        break;
    }
    case LedPattern::FOCUS: g = kMax; break;
    case LedPattern::GRACE: if (slow) { r = kMax; g = kMax * 2 / 3; } break;
    case LedPattern::NUDGE: if (fast) { r = kMax; } break;
    case LedPattern::AWAY: b = kMax; break;
    case LedPattern::BREAK: r = kMax * 2 / 3; b = kMax; break;
    case LedPattern::DONE: hsv((float)(t_ms % 2000) / 2000.0f * 360.0f, kMax, r, g, b); break;
    case LedPattern::CAPTURE: g = kMax; b = kMax; break;
    case LedPattern::ERROR: r = kMax; break;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void led_task(void *)
{
    int low_count = 0;
    bool down = false;
    while (true) {
        const int64_t t_ms = esp_timer_get_time() / 1000;
        if (s_strip != nullptr) {
            render((LedPattern)s_pattern.load(), t_ms);
        }
        if (s_button_gpio >= 0) {
            const bool low = gpio_get_level((gpio_num_t)s_button_gpio) == 0;
            low_count = low ? low_count + 1 : 0;
            if (!down && low_count >= 2) {  // held for 2 polls (~66 ms)
                down = true;
                s_pressed = true;
            } else if (down && !low) {
                down = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

void led_start(int led_gpio, int button_gpio)
{
    led_strip_config_t strip = {};
    strip.strip_gpio_num = led_gpio;
    strip.max_leds = 1;
    strip.led_model = LED_MODEL_WS2812;
    strip.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt = {};
    rmt.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz = 10 * 1000 * 1000;
    if (led_strip_new_rmt_device(&strip, &rmt, &s_strip) != ESP_OK) {
        ESP_LOGE(TAG, "no LED strip on GPIO%d", led_gpio);
        s_strip = nullptr;
    }
    if (button_gpio >= 0) {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << button_gpio;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io);
        s_button_gpio = button_gpio;
    }
    xTaskCreatePinnedToCore(led_task, "led", 3072, nullptr, 5, nullptr, 1);
}

void led_set(LedPattern p)
{
    s_pattern = (int)p;
}

bool led_take_button_press()
{
    return s_pressed.exchange(false);
}
