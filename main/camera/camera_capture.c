#include "camera_capture.h"
#include "camera_tuning.h"
#include "camera_pins.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "camera";
#define CAMERA_MAX_JPEG_BYTES (256U * 1024U)
#define CAMERA_MAX_QUESTION_BYTES 1024U
#define CAMERA_WARMUP_FRAMES 4U
#define CAMERA_WARMUP_GAP_MS 60U
#define CAMERA_STREAM_MAX_US (10LL * 60LL * 1000000LL)

static const camera_config_t s_camera_config = {
    .pin_pwdn = -1,
    .pin_reset = -1,
    .pin_xclk = SENSE_CAMERA_XCLK,
    .pin_sccb_sda = SENSE_CAMERA_SDA,
    .pin_sccb_scl = SENSE_CAMERA_SCL,
    .pin_d0 = SENSE_CAMERA_D0,
    .pin_d1 = SENSE_CAMERA_D1,
    .pin_d2 = SENSE_CAMERA_D2,
    .pin_d3 = SENSE_CAMERA_D3,
    .pin_d4 = SENSE_CAMERA_D4,
    .pin_d5 = SENSE_CAMERA_D5,
    .pin_d6 = SENSE_CAMERA_D6,
    .pin_d7 = SENSE_CAMERA_D7,
    .pin_vsync = SENSE_CAMERA_VSYNC,
    .pin_href = SENSE_CAMERA_HREF,
    .pin_pclk = SENSE_CAMERA_PCLK,
    .xclk_freq_hz = 20000000,
    /* ESP32-S3 generates XCLK via LCD_CAM, not LEDC; these members
     * are required by the shared config but unused on this target. */
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_VGA,
    .jpeg_quality = 12,
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

static StaticSemaphore_t s_camera_mutex_storage;
static SemaphoreHandle_t s_camera_mutex;
static portMUX_TYPE s_camera_init_lock = portMUX_INITIALIZER_UNLOCKED;

static portMUX_TYPE s_camera_state_lock = portMUX_INITIALIZER_UNLOCKED;
static espclaw_camera_settings_t s_settings = {
    .fps = 20, .flicker_hz = 50, .wb_mode = 0, .brightness = 1, .saturation = -2,
};
static unsigned s_settings_revision = 1;
static espclaw_camera_telemetry_t s_telemetry;
const uint16_t espclaw_camera_register_addresses[ESPCLAW_CAMERA_REGISTER_COUNT] = {
    0x3004, 0x300c, 0x303a, 0x303b, 0x303c, 0x303d, 0x3108,
    0x380c, 0x380d, 0x380e, 0x380f, 0x3824, 0x460c,
    0x3a00, 0x3a08, 0x3a09, 0x3a0a, 0x3a0b, 0x3a0d, 0x3a0e,
    0x3c00, 0x3c01, 0x3c0c, 0x3406,
    0x3400, 0x3401, 0x3402, 0x3403, 0x3404, 0x3405,
    0x3500, 0x3501, 0x3502, 0x350a, 0x350b, 0x350c, 0x350d,
    0x5587, 0x5588,
    0x5381, 0x5382, 0x5383, 0x5384, 0x5385, 0x5386,
    0x5387, 0x5388, 0x5389, 0x538a, 0x538b,
};

static SemaphoreHandle_t camera_mutex(void)
{
    portENTER_CRITICAL(&s_camera_init_lock);
    if (!s_camera_mutex)
        s_camera_mutex = xSemaphoreCreateMutexStatic(&s_camera_mutex_storage);
    portEXIT_CRITICAL(&s_camera_init_lock);
    return s_camera_mutex;
}

void espclaw_camera_get_settings(espclaw_camera_settings_t *settings,
                                  espclaw_camera_telemetry_t *telemetry)
{
    portENTER_CRITICAL(&s_camera_state_lock);
    if (settings) *settings = s_settings;
    if (telemetry) {
        *telemetry = s_telemetry;
        telemetry->settings_revision = s_settings_revision;
        telemetry->settings_applied = telemetry->valid &&
            telemetry->applied_revision == s_settings_revision;
    }
    portEXIT_CRITICAL(&s_camera_state_lock);
}

esp_err_t espclaw_camera_set_settings(const espclaw_camera_settings_t *settings)
{
    if (!espclaw_camera_settings_valid(settings)) return ESP_ERR_INVALID_ARG;
    SemaphoreHandle_t mutex = camera_mutex();
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_camera_state_lock);
    if (memcmp(&s_settings, settings, sizeof(*settings)) != 0) {
        s_settings = *settings;
        if (++s_settings_revision == 0) s_settings_revision = 1;
    }
    portEXIT_CRITICAL(&s_camera_state_lock);
    xSemaphoreGive(mutex);
    return ESP_OK;
}

static int sensor_read16(sensor_t *sensor, unsigned address)
{
    int high = sensor->get_reg(sensor, address, 0xff);
    int low = sensor->get_reg(sensor, address + 1U, 0xff);
    return high < 0 || low < 0 ? -1 : (high << 8) | low;
}

static uint32_t sensor_sysclk(sensor_t *sensor)
{
    return espclaw_ov3660_sysclk(sensor->xclk_freq_hz,
        sensor->get_reg(sensor, 0x303a, 0xff), sensor->get_reg(sensor, 0x303b, 0xff),
        sensor->get_reg(sensor, 0x303c, 0xff), sensor->get_reg(sensor, 0x303d, 0xff),
        sensor->get_reg(sensor, 0x3108, 0xff));
}

/* All sensor accesses in these helpers occur while the hardware mutex is held.
 * Register definitions: OV3660 datasheet v1.3 sections 3.4.3/3.4.4, tables7-9/7-11:
 * https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/OV3660_datasheet.pdf
 * No OV5640 register assumptions and no clock/PLL tuning are used here. */
static esp_err_t camera_apply_settings(const espclaw_camera_settings_t *settings)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || sensor->id.PID != OV3660_PID) return ESP_ERR_NOT_SUPPORTED;
    if (!sensor->get_reg || !sensor->set_reg || !sensor->set_brightness ||
        !sensor->set_saturation || !sensor->set_whitebal || !sensor->set_wb_mode ||
        !sensor->set_awb_gain) return ESP_ERR_NOT_SUPPORTED;
    int hts = sensor_read16(sensor, 0x380c);
    int vts = sensor_read16(sensor, 0x380e);
    unsigned step50, step60, max50, max60;
    uint32_t sysclk = sensor_sysclk(sensor);
    if (hts <= 0 || vts <= 0 || !espclaw_ov3660_banding(sysclk, (unsigned)hts,
        (unsigned)vts, &step50, &step60, &max50, &max60)) return ESP_ERR_INVALID_STATE;

    /* Match Espressif's OV3660 brightness/saturation baseline, then apply the
     * user's bounded values. WB presets come from the pinned OV3660 driver. */
    if (sensor->set_brightness(sensor, settings->brightness) ||
        sensor->set_saturation(sensor, settings->saturation) ||
        sensor->set_whitebal(sensor, 1) || sensor->set_awb_gain(sensor, 1) ||
        sensor->set_wb_mode(sensor, settings->wb_mode)) return ESP_FAIL;

    /* Enable banding and leave night mode off. Preserve bit4 (sub-band
     * exposure allowance) so bright scenes are not forced to overexpose. */
    if (sensor->set_reg(sensor, 0x3a00, 0x24, 0x20) ||
        sensor->set_reg(sensor, 0x3a08, 0x03, step50 >> 8) ||
        sensor->set_reg(sensor, 0x3a09, 0xff, step50 & 0xff) ||
        sensor->set_reg(sensor, 0x3a0a, 0x03, step60 >> 8) ||
        sensor->set_reg(sensor, 0x3a0b, 0xff, step60 & 0xff) ||
        sensor->set_reg(sensor, 0x3a0e, 0x3f, max50) ||
        sensor->set_reg(sensor, 0x3a0d, 0x3f, max60)) return ESP_FAIL;
    if (settings->flicker_hz == 0) {
        /* Datasheet3.4.3: XVCLK/0x300C[3:0] should be approximately3MHz. */
        unsigned divider = (sensor->xclk_freq_hz + 1500000U) / 3000000U;
        if (divider < 1U || divider > 15U) return ESP_ERR_INVALID_STATE;
        if (sensor->set_reg(sensor, 0x300c, 0x0f, divider) ||
            sensor->set_reg(sensor, 0x3004, 0x04, 0x04) ||
            sensor->set_reg(sensor, 0x3c01, 0x80, 0)) return ESP_FAIL;
    } else {
        if (sensor->set_reg(sensor, 0x3c00, 0x04, settings->flicker_hz == 50 ? 0x04 : 0) ||
            sensor->set_reg(sensor, 0x3c01, 0x80, 0x80)) return ESP_FAIL;
    }
    return ESP_OK;
}

static int sampled_register(const espclaw_camera_telemetry_t *sample, uint16_t address)
{
    for (unsigned i = 0; i < ESPCLAW_CAMERA_REGISTER_COUNT; ++i)
        if (espclaw_camera_register_addresses[i] == address) return sample->registers[i];
    return -1;
}

static void camera_sample_telemetry(unsigned applied_revision)
{
    sensor_t *sensor = esp_camera_sensor_get();
    espclaw_camera_telemetry_t sample = {0};
    sample.available = sensor != NULL;
    sample.sampled_at_us = esp_timer_get_time();
    sample.applied_revision = applied_revision;
    if (sensor) {
        sample.sensor_pid = sensor->id.PID;
        sample.xclk_hz = sensor->xclk_freq_hz;
    }
    if (sensor && sensor->id.PID == OV3660_PID && sensor->get_reg) {
        sample.valid = true;
        for (unsigned i = 0; i < ESPCLAW_CAMERA_REGISTER_COUNT; ++i) {
            sample.registers[i] = sensor->get_reg(sensor, espclaw_camera_register_addresses[i], 0xff);
            if (sample.registers[i] < 0) sample.valid = false;
        }
        if (sample.valid) {
#define REG(address) sampled_register(&sample, (address))
            sample.sysclk_hz = espclaw_ov3660_sysclk(sample.xclk_hz,
                REG(0x303a), REG(0x303b), REG(0x303c), REG(0x303d), REG(0x3108));
            sample.hts = (REG(0x380c) << 8) | REG(0x380d);
            sample.vts = (REG(0x380e) << 8) | REG(0x380f);
            if (!sample.hts || !sample.vts || !sample.sysclk_hz) sample.valid = false;
            else sample.nominal_sensor_fps = (double)sample.sysclk_hz / sample.hts / sample.vts;
            sample.detected_hz = (REG(0x3c0c) & 1) ? 50 : 60;
            sample.band_step50 = ((REG(0x3a08) & 3) << 8) | REG(0x3a09);
            sample.band_step60 = ((REG(0x3a0a) & 3) << 8) | REG(0x3a0b);
            sample.max_bands50 = REG(0x3a0e) & 0x3f;
            sample.max_bands60 = REG(0x3a0d) & 0x3f;
            sample.banding_enabled = (REG(0x3a00) & 0x20) != 0;
            sample.night_mode = (REG(0x3a00) & 0x04) != 0;
            sample.wb_manual = (REG(0x3406) & 1) != 0;
            sample.exposure_lines = (((REG(0x3500) & 0x0f) << 16) |
                (REG(0x3501) << 8) | REG(0x3502)) / 16.0;
#undef REG
        }
    }
    portENTER_CRITICAL(&s_camera_state_lock);
    s_telemetry = sample;
    portEXIT_CRITICAL(&s_camera_state_lock);
}

bool espclaw_camera_is_busy(void)
{
    /* Creating this synchronization primitive never initializes the sensor. */
    return uxSemaphoreGetCount(camera_mutex()) == 0;
}

/* The caller owns the hardware mutex and initialized sensor. */
static esp_err_t camera_warmup(espclaw_camera_stop_callback_t stop, void *context)
{
    for (unsigned i = 0; i < CAMERA_WARMUP_FRAMES; ++i) {
        if (stop && stop(context)) return ESP_OK;
        camera_fb_t *frame = esp_camera_fb_get();
        if (!frame) return ESP_ERR_TIMEOUT;
        esp_camera_fb_return(frame);
        vTaskDelay(pdMS_TO_TICKS(CAMERA_WARMUP_GAP_MS));
    }
    return ESP_OK;
}

static bool camera_frame_is_jpeg(const camera_fb_t *frame)
{
    return frame->format == PIXFORMAT_JPEG && frame->buf &&
        frame->len >= 2U && frame->len <= CAMERA_MAX_JPEG_BYTES &&
        frame->buf[0] == 0xff && frame->buf[1] == 0xd8;
}

esp_err_t espclaw_camera_capture_jpeg(uint8_t **jpeg_out, size_t *jpeg_size,
                                     uint32_t wait_ms)
{
    if (!jpeg_out || !jpeg_size) return ESP_ERR_INVALID_ARG;
    *jpeg_out = NULL;
    *jpeg_size = 0;
    SemaphoreHandle_t mutex = camera_mutex();
    if (wait_ms > 2000U) wait_ms = 2000U;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
        return ESP_ERR_INVALID_STATE;

    espclaw_camera_settings_t settings;
    espclaw_camera_telemetry_t previous;
    espclaw_camera_get_settings(&settings, &previous);
    unsigned applied_revision = 0;
    camera_fb_t *frame = NULL;
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;
    bool camera_started = false;
    esp_err_t err = esp_camera_init(&s_camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    camera_started = true;
    err = camera_apply_settings(&settings);
    if (err != ESP_OK) goto cleanup;
    applied_revision = previous.settings_revision;
    err = camera_warmup(NULL, NULL);
    if (err != ESP_OK) goto cleanup;
    frame = esp_camera_fb_get();
    if (!frame) {
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    if (!camera_frame_is_jpeg(frame)) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    jpeg = heap_caps_malloc(frame->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    memcpy(jpeg, frame->buf, frame->len);
    jpeg_len = frame->len;
    ESP_LOGI(TAG, "Captured one %ux%u JPEG (%u bytes)",
             (unsigned)frame->width, (unsigned)frame->height, (unsigned)frame->len);

cleanup:
    if (frame) esp_camera_fb_return(frame);
    if (camera_started) {
        camera_sample_telemetry(applied_revision);
        esp_err_t stop_err = esp_camera_deinit();
        if (stop_err != ESP_OK) {
            ESP_LOGE(TAG, "Camera deinit failed: %s", esp_err_to_name(stop_err));
            if (err == ESP_OK) err = stop_err;
        }
    }
    xSemaphoreGive(mutex);
    /* Both HTTP and vision callers receive a copy only after teardown. */
    if (err == ESP_OK) {
        *jpeg_out = jpeg;
        *jpeg_size = jpeg_len;
    } else {
        free(jpeg);
    }
    return err;
}

esp_err_t espclaw_camera_stream_jpeg(espclaw_camera_frame_callback_t on_frame,
                                      espclaw_camera_stop_callback_t stop,
                                      void *context)
{
    if (!on_frame || !stop) return ESP_ERR_INVALID_ARG;
    SemaphoreHandle_t mutex = camera_mutex();
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;

    espclaw_camera_settings_t settings;
    espclaw_camera_telemetry_t previous;
    espclaw_camera_get_settings(&settings, &previous);
    unsigned applied_revision = 0;
    camera_fb_t *frame = NULL;
    bool camera_started = false;
    int64_t deadline = esp_timer_get_time() + CAMERA_STREAM_MAX_US;
    esp_err_t err = ESP_OK;
    if (stop(context)) goto cleanup;
    camera_config_t stream_config = s_camera_config;
    stream_config.fb_count = 2;
    stream_config.grab_mode = CAMERA_GRAB_LATEST;
    err = esp_camera_init(&stream_config);
    if (err != ESP_OK) goto cleanup;
    camera_started = true;
    err = camera_apply_settings(&settings);
    if (err != ESP_OK) goto cleanup;
    applied_revision = previous.settings_revision;
    err = camera_warmup(stop, context);
    if (err != ESP_OK) goto cleanup;

    camera_sample_telemetry(applied_revision);
    while (!stop(context) && esp_timer_get_time() < deadline) {
        int64_t iteration_started = esp_timer_get_time();
        frame = esp_camera_fb_get();
        if (!frame) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (!camera_frame_is_jpeg(frame)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (stop(context) || esp_timer_get_time() >= deadline) break;
        /* The HTTP task sends this borrowed frame before it is returned to
         * the driver. No per-frame allocation, copy, logging or persistence. */
        err = on_frame(frame->buf, frame->len, context);
        esp_camera_fb_return(frame);
        frame = NULL;
        if (err != ESP_OK) break;
        int64_t remaining_us = (1000000LL + settings.fps - 1) / settings.fps -
            (esp_timer_get_time() - iteration_started);
        if (remaining_us > 0) {
            /* Count acquisition and socket writes toward the frame interval;
             * round the remaining delay up to the next FreeRTOS tick. */
            uint32_t remaining_ms = (uint32_t)((remaining_us + 999LL) / 1000LL);
            vTaskDelay(pdMS_TO_TICKS(remaining_ms + portTICK_PERIOD_MS - 1U));
        }
    }

cleanup:
    if (frame) esp_camera_fb_return(frame);
    if (camera_started) {
        camera_sample_telemetry(applied_revision);
        esp_err_t stop_err = esp_camera_deinit();
        if (err == ESP_OK) err = stop_err;
    }
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t espclaw_camera_build_messages(const char *question, char **messages_out)
{
    if (!messages_out) return ESP_ERR_INVALID_ARG;
    *messages_out = NULL;
    if (!question || !question[0])
        question = "請用繁體中文簡短描述呢張相入面見到嘅內容。";
    size_t question_len = strnlen(question, CAMERA_MAX_QUESTION_BYTES + 1U);
    if (question_len > CAMERA_MAX_QUESTION_BYTES) return ESP_ERR_INVALID_SIZE;

    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    char *data_url = NULL;
    char *messages_json = NULL;
    cJSON *messages = NULL;
    esp_err_t err = espclaw_camera_capture_jpeg(&jpeg, &jpeg_size, 2000);
    if (err != ESP_OK) return err;

    static const char prefix[] = "data:image/jpeg;base64,";
    size_t encoded_capacity = 4U * ((jpeg_size + 2U) / 3U) + 1U;
    size_t data_url_capacity = sizeof(prefix) - 1U + encoded_capacity;
    data_url = heap_caps_malloc(data_url_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data_url) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    memcpy(data_url, prefix, sizeof(prefix) - 1U);
    size_t encoded_len = 0;
    int encode_result = mbedtls_base64_encode(
        (unsigned char *)data_url + sizeof(prefix) - 1U, encoded_capacity,
        &encoded_len, jpeg, jpeg_size);
    if (encode_result != 0) {
        err = ESP_FAIL;
        goto cleanup;
    }
    free(jpeg);
    jpeg = NULL;
    data_url[sizeof(prefix) - 1U + encoded_len] = '\0';

    messages = cJSON_CreateArray();
    cJSON *message = cJSON_CreateObject();
    if (!messages || !message) {
        cJSON_Delete(message);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    cJSON_AddItemToArray(messages, message);
    cJSON *content = cJSON_AddArrayToObject(message, "content");
    if (!cJSON_AddStringToObject(message, "role", "user") || !content) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    cJSON *text_part = cJSON_CreateObject();
    cJSON *image_part = cJSON_CreateObject();
    if (!text_part || !image_part) {
        cJSON_Delete(text_part);
        cJSON_Delete(image_part);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    cJSON_AddItemToArray(content, text_part);
    cJSON_AddItemToArray(content, image_part);
    cJSON *image_url = cJSON_AddObjectToObject(image_part, "image_url");
    if (!cJSON_AddStringToObject(text_part, "type", "text") ||
        !cJSON_AddStringToObject(text_part, "text", question) ||
        !cJSON_AddStringToObject(image_part, "type", "image_url") || !image_url) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    /* A reference avoids duplicating the large base64 image in cJSON. */
    cJSON *url = cJSON_CreateStringReference(data_url);
    if (!url) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (!cJSON_AddItemToObject(image_url, "url", url)) {
        cJSON_Delete(url);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    size_t json_capacity = data_url_capacity + question_len * 6U + 256U;
    messages_json = heap_caps_malloc(json_capacity,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!messages_json) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (!cJSON_PrintPreallocated(messages, messages_json,
                                (int)json_capacity, false)) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    *messages_out = messages_json;
    messages_json = NULL;
    err = ESP_OK;

cleanup:
    cJSON_Delete(messages);
    free(jpeg);
    free(messages_json);
    free(data_url);
    return err;
}
