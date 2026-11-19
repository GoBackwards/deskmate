#include "settings.h"

#include "nvs.h"
#include "nvs_flash.h"

static constexpr uint32_t kVersion = 2;
static const char *kNs = "deskmate";
static const char *kKey = "settings";

void settings_defaults(Settings &s)
{
    s = {};
    s.version = kVersion;
    s.roi[0] = 100;  // bottom-centre of the frame until the pad is marked
    s.roi[1] = 150;
    s.roi[2] = 220;
    s.roi[3] = 236;
    s.session_s = 25 * 60;
    s.grace_s = 10;
    s.away_s = 5;
    s.renudge_s = 30;
    s.phone_thr = 0.5f;
    s.person_thr = 0.5f;
    s.person_min_area = 0.05f;
    s.person_enabled = 1;
}

bool settings_load(Settings &s)
{
    settings_defaults(s);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    Settings tmp;
    size_t len = sizeof(tmp);
    const bool ok = nvs_get_blob(h, kKey, &tmp, &len) == ESP_OK && len == sizeof(tmp) && tmp.version == kVersion;
    nvs_close(h);
    if (ok) {
        s = tmp;
    }
    return ok;
}

bool settings_save(const Settings &s)
{
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_blob(h, kKey, &s, sizeof(s)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
