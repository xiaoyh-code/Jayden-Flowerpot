#ifndef ESPCLAW_CAMERA_CAPTURE_H
#define ESPCLAW_CAMERA_CAPTURE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fps;            /* Stream delivery cap: 10, 15, 20 or 25. */
    int flicker_hz;     /* 0=auto, 50 or 60. */
    int wb_mode;        /* Driver modes: 0=auto, 1=daylight, 3=office, 4=home. */
    int brightness;    /* -2 through 2; zero is neutral. */
    int saturation;    /* -2 through 2; zero is neutral. */
} espclaw_camera_settings_t;

#define ESPCLAW_CAMERA_REGISTER_COUNT 56U
extern const uint16_t espclaw_camera_register_addresses[ESPCLAW_CAMERA_REGISTER_COUNT];
typedef struct {
    bool available, valid, settings_applied;
    unsigned sensor_pid, settings_revision, applied_revision;
    int64_t sampled_at_us;
    uint32_t xclk_hz, sysclk_hz;
    unsigned hts, vts;
    double nominal_sensor_fps;
    int detected_hz, band_step50, band_step60, max_bands50, max_bands60;
    bool banding_enabled, night_mode, wb_manual;
    bool banding_auto;
    int selected_hz;    /* Manual selection, or detector result in auto mode. */
    double exposure_lines;
    int registers[ESPCLAW_CAMERA_REGISTER_COUNT]; /* -1 if not sampled. */
} espclaw_camera_telemetry_t;

/* Pure state access: never initializes or accesses the physical sensor.
 * Settings are RAM-only and apply at the next explicit capture/stream init.
 * set_settings returns INVALID_STATE while camera hardware is in use. */
void espclaw_camera_get_settings(espclaw_camera_settings_t *settings,
                                  espclaw_camera_telemetry_t *telemetry);
esp_err_t espclaw_camera_set_settings(const espclaw_camera_settings_t *settings);

/* Explicit-request API shared by serial /look and authenticated HTTP.
 * Returns a PSRAM JPEG copy after stopping the camera; caller must free().
 * wait_ms is bounded to 2000; ESP_ERR_INVALID_STATE means camera busy. */
esp_err_t espclaw_camera_capture_jpeg(uint8_t **jpeg_out, size_t *jpeg_size,
                                     uint32_t wait_ms);
bool espclaw_camera_is_busy(void);

typedef esp_err_t (*espclaw_camera_frame_callback_t)(const uint8_t *jpeg,
                                                     size_t jpeg_size,
                                                     void *context);
typedef bool (*espclaw_camera_stop_callback_t)(void *context);

/* Explicit live preview: initialize once, discard four startup frames, then
 * deliver borrowed VGA JPEGs at the configured cap for at most 10 minutes.
 * The callback must finish using each frame before returning. Stop is checked
 * between frames; disconnect/error/stop always tears down and unlocks camera.
 * No images are stored. Returns INVALID_STATE if another capture is active. */
esp_err_t espclaw_camera_stream_jpeg(espclaw_camera_frame_callback_t on_frame,
                                      espclaw_camera_stop_callback_t stop,
                                      void *context);

/* Discard bounded startup frames, then capture one VGA JPEG for analysis and
 * produce one OpenAI messages array with the question and image data URL.
 * The camera is stopped before returning;
 * neither the JPEG nor the resulting JSON is saved to disk/NVS/history.
 * Caller owns *messages_out and must free() it after the provider call.
 * Must only be invoked for an explicit local /look command. */
esp_err_t espclaw_camera_build_messages(const char *question,
                                        char **messages_out);

#endif
