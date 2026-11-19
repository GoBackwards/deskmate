// Deskmate midterm demo for the Freenove ESP32-S3-WROOM CAM (FNK0085, GC0308 camera).
//
// Every frame (all on the ESP32-S3):
//   camera RGB565 320x240
//   -> pad CNN (int8): is the phone parked on the pad region?
//   -> YOLO11n COCO (int8): is someone sitting at the desk?
//   -> focus state machine (focus_fsm.cpp) -> RGB LED pattern + event lines.
// Serial at 921600 (CH343 port), one line each:
//   DET <frame> person pre=<ms> model=<ms> post=<ms> n=<k> | <class> <score> x1 y1 x2 y2 | ...
//   ST <frame> <STATE> focus=<ms> target=<ms> grace=<ms> pickups=<n> phone=<0/1> p=<p> person=<0/1>
//      ps=<score> pad_ms=<ms> det_ms=<ms> img_ms=<ms> enc_ms=<ms> cycle_ms=<ms> led=<name>
//   EV <frame> <EVENT> focus=<ms> pickups=<n>
//   IMG <frame> <base64 JPEG>
//   CFG ...   (after boot and after every settings change)
// Commands (one per line): r x1 y1 x2 y2 | s session grace away [renudge] | t phone_thr person_thr min_area
//   c0/c1 capture mode | i0/i1 images | q<quality> | p0/p1 person detector | n new session
//   b break | d defaults | ?
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "dl_image_jpeg.hpp"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#include "cam.h"
#include "focus_fsm.h"
#include "led.h"
#include "pad_classifier.hpp"
#include "settings.h"
#include "yolo_detector.hpp"

static constexpr const char *kVersion = "0.2";
static constexpr const char *kPartition = "models";
static constexpr const char *kPersonModel = "coco_detect_yolo11n_320_s8_v1.espdl";
static constexpr const char *kPadModel = "pad_cls_96_s8.espdl";
static constexpr int kLedGpio = 48;
static constexpr int kButtonGpio = 0;  // BOOT button = break
static constexpr int kW = 320;
static constexpr int kH = 240;
static constexpr uart_port_t kUart = UART_NUM_0;

static Settings g_set;
static FocusFsm g_fsm;
static Yolo11Detector *g_person = nullptr;
static PadClassifier *g_pad = nullptr;
static bool g_capture = false;
static bool g_send_img = true;
static int g_jpeg_q = 80;  // q80 costs ~2 ms more than q60 to encode (35 vs 37 ms)
static constexpr int kCaptureJpegQ = 90;
static uint32_t g_frame = 0;

// ------------------------------------------------------------------ output
// Protocol lines go to the UART driver in ONE call each. printf through ESP-IDF's VFS hands
// the driver one byte at a time (~28 us per byte): a 10 KB image line took ~300 ms that way.
static char g_line[1024];
static int g_len = 0;

static void lb_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lb_add(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int room = (int)sizeof(g_line) - 1 - g_len;
    const int n = vsnprintf(g_line + g_len, room > 0 ? room : 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        g_len += n < room ? n : (room > 0 ? room - 1 : 0);
    }
}

static void lb_send()
{
    fflush(stdout);  // keep order with any ESP_LOG text still buffered in stdio
    g_line[g_len++] = '\n';
    uart_write_bytes(kUart, g_line, g_len);
    g_len = 0;
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(g_line, sizeof(g_line) - 1, fmt, ap);
    va_end(ap);
    g_len = n < (int)sizeof(g_line) - 1 ? n : (int)sizeof(g_line) - 2;
    lb_send();
}

static uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool model_present(const char *name)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartition);
    if (p == nullptr) {
        return false;
    }
    const size_t len = p->size < 8192 ? p->size : 8192;
    char *head = (char *)malloc(len);
    bool found = false;
    if (head != nullptr && esp_partition_read(p, 0, head, len) == ESP_OK) {
        found = memmem(head, len, name, strlen(name)) != nullptr;
    }
    free(head);
    return found;
}

static void apply_settings()
{
    FocusConfig &c = g_fsm.config();
    c.session_ms = g_set.session_s * 1000u;
    c.grace_ms = g_set.grace_s * 1000u;
    c.away_ms = g_set.away_s * 1000u;
    c.renudge_ms = g_set.renudge_s * 1000u;
    // The loop runs ~5 times a second with the person detector off: 3 frames = ~0.6 s.
    c.phone_on_frames = 3;
    c.phone_off_frames = 3;
}

static void print_cfg()
{
    out("CFG version=%s roi=%d,%d,%d,%d session=%lu grace=%lu away=%lu renudge=%lu phone_thr=%.2f "
        "person_thr=%.2f min_area=%.3f person=%d pad_model=%d person_model=%d",
        kVersion, g_set.roi[0], g_set.roi[1], g_set.roi[2], g_set.roi[3], (unsigned long)g_set.session_s,
        (unsigned long)g_set.grace_s, (unsigned long)g_set.away_s, (unsigned long)g_set.renudge_s, g_set.phone_thr,
        g_set.person_thr, g_set.person_min_area, g_set.person_enabled, g_pad != nullptr, g_person != nullptr);
}

static void print_heap(const char *when)
{
    out("HEAP %s psram_free=%u internal_free=%u internal_largest=%u", when,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void emit(FocusEvent e)
{
    if (e != FocusEvent::NONE) {
        out("EV %lu %s focus=%lu pickups=%d", (unsigned long)g_frame, event_name(e), (unsigned long)g_fsm.focus_ms(),
            g_fsm.pickups());
    }
}

static LedPattern pattern_for(FocusState s)
{
    switch (s) {
    case FocusState::IDLE: return LedPattern::IDLE;
    case FocusState::FOCUS: return LedPattern::FOCUS;
    case FocusState::GRACE: return LedPattern::GRACE;
    case FocusState::NUDGE: return LedPattern::NUDGE;
    case FocusState::AWAY: return LedPattern::AWAY;
    case FocusState::BREAK: return LedPattern::BREAK;
    case FocusState::DONE: return LedPattern::DONE;
    }
    return LedPattern::ERROR;
}

// JPEG with Espressif's esp_new_jpeg encoder (through ESP-DL): ~35 ms for 320x240 on the S3.
static int64_t g_img_us = 0;  // time of the last send_jpeg, reported in the next ST line
static int64_t g_enc_us = 0;  // of which JPEG encoding

static void send_jpeg(const dl::image::img_t &img, int quality)
{
    const int64_t t0 = esp_timer_get_time();
    dl::image::jpeg_img_t jpg = dl::image::sw_encode_jpeg(img, (uint8_t)quality);
    g_enc_us = esp_timer_get_time() - t0;
    if (jpg.data == nullptr) {
        out("ERR jpeg");
        return;
    }
    size_t b64_len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64_len, (const unsigned char *)jpg.data, jpg.data_len);
    char head[24];
    const int head_len = snprintf(head, sizeof(head), "IMG %lu ", (unsigned long)g_frame);
    // One buffer = one driver call, so no other line can land inside the image line.
    char *buf = (char *)heap_caps_malloc(head_len + b64_len + 2, MALLOC_CAP_SPIRAM);
    if (buf != nullptr) {
        memcpy(buf, head, head_len);
        size_t written = 0;
        if (mbedtls_base64_encode((unsigned char *)buf + head_len, b64_len + 1, &written,
                                  (const unsigned char *)jpg.data, jpg.data_len) == 0) {
            buf[head_len + written] = '\n';
            fflush(stdout);
            uart_write_bytes(kUart, buf, head_len + written + 1);
        }
        free(buf);
    }
    heap_caps_free(jpg.data);
    g_img_us = esp_timer_get_time() - t0;
}

static void set_roi(int x1, int y1, int x2, int y2)
{
    x1 = x1 < 0 ? 0 : (x1 > kW - 16 ? kW - 16 : x1);
    y1 = y1 < 0 ? 0 : (y1 > kH - 16 ? kH - 16 : y1);
    x2 = x2 > kW ? kW : (x2 < x1 + 16 ? x1 + 16 : x2);
    y2 = y2 > kH ? kH : (y2 < y1 + 16 ? y1 + 16 : y2);
    g_set.roi[0] = x1;
    g_set.roi[1] = y1;
    g_set.roi[2] = x2;
    g_set.roi[3] = y2;
}

static void handle_command(char *cmd)
{
    int a = 0, b = 0, c = 0, d = 0;
    float fa = 0, fb = 0, fc = 0;
    switch (cmd[0]) {
    case 'r':
        if (sscanf(cmd + 1, "%d %d %d %d", &a, &b, &c, &d) == 4) {
            set_roi(a, b, c, d);
            settings_save(g_set);
        }
        print_cfg();
        break;
    case 's': {
        const int n = sscanf(cmd + 1, "%d %d %d %d", &a, &b, &c, &d);
        if (n >= 3 && a > 0 && b >= 0 && c > 0) {
            g_set.session_s = a;
            g_set.grace_s = b;
            g_set.away_s = c;
            if (n == 4 && d > 0) {
                g_set.renudge_s = d;
            }
            apply_settings();
            settings_save(g_set);
        }
        print_cfg();
        break;
    }
    case 't':
        if (sscanf(cmd + 1, "%f %f %f", &fa, &fb, &fc) == 3) {
            g_set.phone_thr = fa;
            g_set.person_thr = fb;
            g_set.person_min_area = fc;
            settings_save(g_set);
        }
        print_cfg();
        break;
    case 'c':
        g_capture = cmd[1] == '1';
        led_set(g_capture ? LedPattern::CAPTURE : pattern_for(g_fsm.state()));
        out("OK capture=%d", g_capture);
        break;
    case 'i':
        g_send_img = cmd[1] == '1';
        out("OK images=%d", g_send_img);
        break;
    case 'q':
        g_jpeg_q = atoi(cmd + 1);
        out("OK jpeg_quality=%d", g_jpeg_q);
        break;
    case 'p':
        g_set.person_enabled = cmd[1] == '1';
        settings_save(g_set);
        print_cfg();
        break;
    case 'n':
        emit(g_fsm.reset());
        break;
    case 'b':
        emit(g_fsm.toggle_break(now_ms()));
        break;
    case 'd':
        settings_defaults(g_set);
        apply_settings();
        settings_save(g_set);
        print_cfg();
        break;
    case '?':
        print_cfg();
        break;
    default:
        out("ERR unknown command '%s'", cmd);
    }
}

static void poll_commands()
{
    static char line[96];
    static int n = 0;
    uint8_t buf[64];
    const int got = uart_read_bytes(kUart, buf, sizeof(buf), 0);
    for (int i = 0; i < got; i++) {
        const char ch = (char)buf[i];
        if (ch == '\n' || ch == '\r') {
            if (n > 0) {
                line[n] = 0;
                handle_command(line);
                n = 0;
            }
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = ch;
        }
    }
}

extern "C" void app_main(void)
{
    // UART driver with a big TX buffer: a whole image line is queued and sent in the background.
    uart_driver_install(kUart, 2048, 32768, 0, nullptr, 0);
    uart_vfs_dev_use_driver(kUart);
    led_start(kLedGpio, kButtonGpio);
    led_set(LedPattern::BOOT);
    out("BOOT deskmate_demo %s", kVersion);

    const bool loaded = settings_load(g_set);
    apply_settings();
    out("SETTINGS %s", loaded ? "loaded" : "defaults");
    print_heap("boot");

    if (!cam_init(PIXFORMAT_RGB565, FRAMESIZE_QVGA, 2)) {
        led_set(LedPattern::ERROR);
        out("ERR camera");
        return;
    }
    if (model_present(kPadModel)) {
        const int64_t t0 = esp_timer_get_time();
        g_pad = new PadClassifier(kPartition, kPadModel);
        out("MODEL pad input=%dx%d load_ms=%lld", g_pad->input_w(), g_pad->input_h(),
            (long long)((esp_timer_get_time() - t0) / 1000));
    } else {
        out("WARN no pad model (%s): capture mode works, the timer will not start", kPadModel);
    }
    if (model_present(kPersonModel)) {
        const int64_t t0 = esp_timer_get_time();
        g_person = new Yolo11Detector(kPartition, kPersonModel, 0.25f, 0.7f, true);
        out("MODEL person input=%dx%d load_ms=%lld", g_person->input_w(), g_person->input_h(),
            (long long)((esp_timer_get_time() - t0) / 1000));
    } else {
        out("WARN no person model (%s): person assumed present", kPersonModel);
    }
    print_heap("models");
    print_cfg();

    // Let auto exposure and white balance settle: the first frames after power-up are dark
    // and read p(phone) ~0.5 on an empty bay, which started a false session (2026-11-18).
    const int64_t warm_end = esp_timer_get_time() + 1500 * 1000;
    int warm = 0;
    while (esp_timer_get_time() < warm_end) {
        camera_fb_t *w = esp_camera_fb_get();
        if (w != nullptr) {
            esp_camera_fb_return(w);
            warm++;
        }
    }
    out("WARMUP frames=%d", warm);
    led_set(pattern_for(g_fsm.state()));

    int64_t last_cycle = esp_timer_get_time();
    while (true) {
        poll_commands();
        if (led_take_button_press()) {
            emit(g_fsm.toggle_break(now_ms()));
        }
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == nullptr) {
            out("ERR no frame");
            continue;
        }
        dl::image::img_t img = {.data = fb->buf,
                                .width = (uint16_t)fb->width,
                                .height = (uint16_t)fb->height,
                                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE};
        if (g_capture) {
            send_jpeg(img, kCaptureJpegQ);
            esp_camera_fb_return(fb);
            g_frame++;
            continue;
        }

        float p_phone = 0.0f;
        int64_t pad_us = 0;
        if (g_pad != nullptr) {
            const int64_t t0 = esp_timer_get_time();
            p_phone = g_pad->run(img, {g_set.roi[0], g_set.roi[1], g_set.roi[2], g_set.roi[3]});
            pad_us = esp_timer_get_time() - t0;
        }

        bool person_raw = true;  // no detector: assume someone is there
        float person_best = 0.0f;
        int64_t det_us = 0;
        if (g_person != nullptr && g_set.person_enabled) {
            Yolo11Detector::Timing t;
            std::list<dl::detect::result_t> &res = g_person->run_timed(img, t);
            det_us = t.pre_us + t.model_us + t.post_us;
            person_raw = false;
            const float min_area = g_set.person_min_area * fb->width * fb->height;
            lb_add("DET %lu person pre=%.1f model=%.1f post=%.1f n=%d", (unsigned long)g_frame, t.pre_us / 1000.0,
                   t.model_us / 1000.0, t.post_us / 1000.0, (int)res.size());
            for (const auto &r : res) {
                lb_add(" | %d %.3f %d %d %d %d", r.category, r.score, r.box[0], r.box[1], r.box[2], r.box[3]);
                const float area = (float)(r.box[2] - r.box[0]) * (float)(r.box[3] - r.box[1]);
                if (r.category == 0 && r.score >= g_set.person_thr && area >= min_area) {
                    person_raw = true;
                    person_best = r.score > person_best ? r.score : person_best;
                }
            }
            lb_send();
        }

        const uint32_t t_ms = now_ms();
        const bool phone_raw = g_pad != nullptr && p_phone >= g_set.phone_thr;
        emit(g_fsm.step(t_ms, phone_raw, person_raw));
        const LedPattern led = pattern_for(g_fsm.state());
        led_set(led);

        const int64_t now_us = esp_timer_get_time();
        out("ST %lu %s focus=%lu target=%lu grace=%lu pickups=%d phone=%d p=%.3f person=%d ps=%.2f pad_ms=%.1f "
            "det_ms=%.1f img_ms=%.1f enc_ms=%.1f cycle_ms=%.0f led=%s",
            (unsigned long)g_frame, state_name(g_fsm.state()), (unsigned long)g_fsm.focus_ms(),
            (unsigned long)g_fsm.config().session_ms, (unsigned long)g_fsm.grace_left_ms(t_ms), g_fsm.pickups(),
            g_fsm.phone_on(), p_phone, g_fsm.person_on(), person_best, pad_us / 1000.0, det_us / 1000.0,
            g_img_us / 1000.0, g_enc_us / 1000.0, (now_us - last_cycle) / 1000.0, led_name(led));
        last_cycle = now_us;
        if (g_send_img) {
            send_jpeg(img, g_jpeg_q);
        } else {
            g_img_us = 0;
        }
        esp_camera_fb_return(fb);
        g_frame++;
    }
}
