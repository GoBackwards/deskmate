#include "cam.h"

#include "esp_log.h"
#include "soc/lcd_cam_struct.h"

static const char *TAG = "cam";

// Freenove ESP32-S3-WROOM CAM (FNK0085) camera pins, same as assignment3/firmware/hand_fitts/config.h.
static constexpr int PIN_PWDN = -1;
static constexpr int PIN_RESET = -1;
static constexpr int PIN_XCLK = 15;
static constexpr int PIN_SIOD = 4;
static constexpr int PIN_SIOC = 5;
static constexpr int PIN_D0 = 11;  // esp32-camera calls D0..D7 "Y2".."Y9"
static constexpr int PIN_D1 = 9;
static constexpr int PIN_D2 = 8;
static constexpr int PIN_D3 = 10;
static constexpr int PIN_D4 = 12;
static constexpr int PIN_D5 = 18;
static constexpr int PIN_D6 = 17;
static constexpr int PIN_D7 = 16;
static constexpr int PIN_VSYNC = 6;
static constexpr int PIN_HREF = 7;
static constexpr int PIN_PCLK = 13;

bool cam_init(pixformat_t format, framesize_t size, int fb_count)
{
    camera_config_t c = {};
    c.pin_pwdn = PIN_PWDN;
    c.pin_reset = PIN_RESET;
    c.pin_xclk = PIN_XCLK;
    c.pin_sccb_sda = PIN_SIOD;
    c.pin_sccb_scl = PIN_SIOC;
    c.pin_d7 = PIN_D7;
    c.pin_d6 = PIN_D6;
    c.pin_d5 = PIN_D5;
    c.pin_d4 = PIN_D4;
    c.pin_d3 = PIN_D3;
    c.pin_d2 = PIN_D2;
    c.pin_d1 = PIN_D1;
    c.pin_d0 = PIN_D0;
    c.pin_vsync = PIN_VSYNC;
    c.pin_href = PIN_HREF;
    c.pin_pclk = PIN_PCLK;
    c.xclk_freq_hz = 20000000;
    c.ledc_timer = LEDC_TIMER_0;  // ignored on the S3 (LCD_CAM makes XCLK), but must be valid
    c.ledc_channel = LEDC_CHANNEL_0;
    c.pixel_format = format;
    c.frame_size = size;
    c.jpeg_quality = 12;
    c.fb_count = fb_count;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (check the camera ribbon)", err);
        return false;
    }

    // Assignment 3 finding (2026-11-17): with the S3 LCD_CAM VSYNC input filter on, the
    // GC0308 lost 0.3-20 % of its frames; with it off, none.
    LCD_CAM.cam_ctrl1.cam_vsync_filter_en = 0;
    LCD_CAM.cam_ctrl.cam_update = 1;

    sensor_t *s = esp_camera_sensor_get();
    if (s != nullptr) {
        ESP_LOGI(TAG, "sensor PID 0x%02x, %dx%d", s->id.PID, resolution[size].width, resolution[size].height);
    }
    return true;
}
