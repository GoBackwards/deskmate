#pragma once
#include "esp_camera.h"

// Starts the camera of the Freenove ESP32-S3-WROOM CAM board (GC0308 sensor on this unit).
// Returns false and logs the reason on failure.
bool cam_init(pixformat_t format, framesize_t size, int fb_count);
