#include "channel/channel.h"
#include "channel/channel_http.h"
#include "channel/http_protocol.h"
#include "bus/message_bus.h"
#include "messages.h"
#include "mem/nvs_manager.h"
#include "nvs_keys.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_ESPCLAW_CAMERA_SENSE
#include "camera/camera_capture.h"
#include "camera/camera_tuning.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#endif

#define HTTP_BODY_LIMIT 8192U
#define HTTP_JOB_TIMEOUT_US (180LL * 1000000LL)
#define HTTP_JOB_RETENTION_US (180LL * 1000000LL)
static const char *TAG = "http_channel";
static httpd_handle_t s_server;
#ifdef CONFIG_ESPCLAW_CAMERA_SENSE
static httpd_handle_t s_stream_server;
#endif
static message_bus_t *s_bus;
static SemaphoreHandle_t s_job_mutex;
/* Protected by s_job_mutex, including admission before camera warmup. */
static bool s_capture_active;
static bool s_stream_active;
static bool s_stream_stop;
static char s_token[LLM_API_KEY_BUF_SIZE];
static struct {
    int64_t id;
    int64_t last_id;
    int64_t started_us;
    int64_t finished_us;
    bool pending;
    char reply[CHANNEL_TX_BUF_SIZE];
} s_job;

static bool job_lock(void)
{
    return s_job_mutex && xSemaphoreTake(s_job_mutex, pdMS_TO_TICKS(200)) == pdTRUE;
}

/* Must be called with s_job_mutex held. A late reply cannot revive an expired
 * job; monotonically increasing IDs prevent old replies matching new jobs. */
static void job_expire(void)
{
    int64_t now = esp_timer_get_time();
    if (s_job.pending && now - s_job.started_us >= HTTP_JOB_TIMEOUT_US) {
        s_job.pending = false;
        s_job.finished_us = now;
        snprintf(s_job.reply, sizeof(s_job.reply), "[error] Request timed out after 180 seconds.");
    }
    if (!s_job.pending && s_job.id && now - s_job.finished_us >= HTTP_JOB_RETENTION_US) {
        s_job.id = 0;
        s_job.reply[0] = '\0';
    }
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *object)
{
    char *body = object ? cJSON_PrintUnformatted(object) : NULL;
    cJSON_Delete(object);
    httpd_resp_set_status(req, body ? status : "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body ? body : "{\"ok\":false,\"error\":\"Out of memory\"}", HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *error)
{
    cJSON *object = cJSON_CreateObject();
    if (object) {
        cJSON_AddBoolToObject(object, "ok", false);
        cJSON_AddStringToObject(object, "error", error);
    }
    return send_json(req, status, object);
}

static bool authenticated(httpd_req_t *req)
{
    char header[LLM_API_KEY_BUF_SIZE + 8];
    size_t header_size = httpd_req_get_hdr_value_len(req, "Authorization");
    bool valid = header_size == strlen(s_token) + 7U && header_size < sizeof(header) &&
        httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        memcmp(header, "Bearer ", 7U) == 0 && http_token_equal(header + 7, s_token);
    if (!valid) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        httpd_resp_set_hdr(req, "Connection", "close");
        send_error(req, "401 Unauthorized", "Bearer authentication required");
    }
    return valid;
}

static bool camera_busy(void)
{
#ifdef CONFIG_ESPCLAW_CAMERA_SENSE
    return espclaw_camera_is_busy();
#else
    return false;
#endif
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_FAIL;
    if (!job_lock()) return send_error(req, "503 Service Unavailable", "State is busy");
    job_expire();
    bool streaming = s_stream_active;
    bool busy = s_job.pending || s_capture_active || streaming;
    xSemaphoreGive(s_job_mutex);
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) esp_netif_get_ip_info(netif, &ip_info);
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
    cJSON *object = cJSON_CreateObject();
    if (object) {
        cJSON_AddBoolToObject(object, "ok", true);
        cJSON_AddStringToObject(object, "device", "XIAO ESP32S3 Sense");
        cJSON_AddStringToObject(object, "ip", ip);
        cJSON_AddNumberToObject(object, "heap_free", esp_get_free_heap_size());
#ifdef CONFIG_ESPCLAW_CAMERA_SENSE
        cJSON_AddBoolToObject(object, "camera", true);
#else
        cJSON_AddBoolToObject(object, "camera", false);
#endif
        cJSON_AddBoolToObject(object, "busy", busy || camera_busy());
        cJSON_AddBoolToObject(object, "streaming", streaming);
    }
    return send_json(req, "200 OK", object);
}

/* Reading is bounded by both content length and an absolute deadline. */
static char *read_body(httpd_req_t *req, size_t limit)
{
    if (req->content_len > limit) {
        send_error(req, "413 Payload Too Large", "Request body is too large");
        return NULL;
    }
    char *body = calloc(1, req->content_len + 1U);
    if (!body) {
        send_error(req, "503 Service Unavailable", "Out of memory");
        return NULL;
    }
    size_t offset = 0;
    int64_t deadline = esp_timer_get_time() + 5000000LL;
    while (offset < req->content_len) {
        if (esp_timer_get_time() >= deadline) break;
        int received = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (received <= 0) break;
        offset += (size_t)received;
    }
    if (offset != req->content_len || memchr(body, '\0', offset)) {
        free(body);
        send_error(req, "400 Bad Request", "Incomplete or invalid request body");
        return NULL;
    }
    return body;
}

static bool empty_request_body(httpd_req_t *req)
{
    char *body = read_body(req, 16U);
    if (!body) return false;
    bool empty = body[0] == '\0';
    cJSON *parsed = empty ? NULL : cJSON_ParseWithOpts(body, NULL, true);
    bool valid = empty || (cJSON_IsObject(parsed) && cJSON_GetArraySize(parsed) == 0);
    cJSON_Delete(parsed);
    free(body);
    if (!valid) send_error(req, "400 Bad Request", "Request body must be empty or {}");
    return valid;
}

/* No long operation holds the job mutex. Cleanup must reliably clear its
 * reservation even if another task is briefly posting an inbound message. */
static void release_camera_reservation(bool stream)
{
    xSemaphoreTake(s_job_mutex, portMAX_DELAY);
    if (stream) {
        s_stream_active = false;
        s_stream_stop = false;
    } else {
        s_capture_active = false;
    }
    xSemaphoreGive(s_job_mutex);
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_FAIL;
    if (!empty_request_body(req)) return ESP_FAIL;
    if (!job_lock()) return send_error(req, "503 Service Unavailable", "State is busy");
    job_expire();
    bool busy = s_job.pending || s_capture_active || s_stream_active || camera_busy();
    if (!busy) s_capture_active = true;
    xSemaphoreGive(s_job_mutex);
    if (busy) return send_error(req, "409 Conflict", "Device is busy");
#ifdef CONFIG_ESPCLAW_CAMERA_SENSE
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    esp_err_t err = espclaw_camera_capture_jpeg(&jpeg, &jpeg_size, 0);
    release_camera_reservation(false);
    if (err != ESP_OK) {
        return send_error(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "503 Service Unavailable",
                          err == ESP_ERR_INVALID_STATE ? "Camera is busy" : esp_err_to_name(err));
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, (const char *)jpeg, (ssize_t)jpeg_size);
    free(jpeg);
    return err;
#else
    release_camera_reservation(false);
    return send_error(req, "503 Service Unavailable", "Camera is disabled");
#endif
}

static esp_err_t stream_stop_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_FAIL;
    if (!empty_request_body(req)) return ESP_FAIL;
    if (!job_lock()) return send_error(req, "503 Service Unavailable", "State is busy");
    bool stopping = s_stream_active;
    if (stopping) s_stream_stop = true;
    xSemaphoreGive(s_job_mutex);
    cJSON *object = cJSON_CreateObject();
    if (object) {
        cJSON_AddBoolToObject(object, "ok", true);
        cJSON_AddBoolToObject(object, "stopping", stopping);
    }
    return send_json(req, "200 OK", object);
}

#ifdef CONFIG_ESPCLAW_CAMERA_SENSE
static esp_err_t camera_settings_response(httpd_req_t *req)
{
    espclaw_camera_settings_t settings;
    espclaw_camera_telemetry_t telemetry;
    espclaw_camera_get_settings(&settings, &telemetry);
    cJSON *object = cJSON_CreateObject();
    cJSON *config = object ? cJSON_AddObjectToObject(object, "settings") : NULL;
    cJSON *sample = object ? cJSON_AddObjectToObject(object, "telemetry") : NULL;
    cJSON *registers = sample ? cJSON_AddObjectToObject(sample, "registers") : NULL;
    if (!config || !sample || !registers) {
        cJSON_Delete(object);
        return send_json(req, "503 Service Unavailable", NULL);
    }
    cJSON_AddBoolToObject(object, "ok", true);
    cJSON_AddNumberToObject(config, "fps", settings.fps);
    cJSON_AddStringToObject(config, "flicker_hz", settings.flicker_hz == 50 ? "50" :
        settings.flicker_hz == 60 ? "60" : "auto");
    cJSON_AddStringToObject(config, "wb_mode", settings.wb_mode == 1 ? "daylight" :
        settings.wb_mode == 3 ? "office" : settings.wb_mode == 4 ? "home" : "auto");
    cJSON_AddNumberToObject(config, "brightness", settings.brightness);
    cJSON_AddNumberToObject(config, "saturation", settings.saturation);
#define ADD_BOOL(name) cJSON_AddBoolToObject(sample, #name, telemetry.name)
#define ADD_NUMBER(name) cJSON_AddNumberToObject(sample, #name, telemetry.name)
    ADD_BOOL(available); ADD_BOOL(valid); ADD_BOOL(settings_applied);
    ADD_NUMBER(sensor_pid); ADD_NUMBER(settings_revision); ADD_NUMBER(applied_revision);
    ADD_NUMBER(sampled_at_us); ADD_NUMBER(xclk_hz); ADD_NUMBER(sysclk_hz);
    ADD_NUMBER(hts); ADD_NUMBER(vts); ADD_NUMBER(nominal_sensor_fps);
    ADD_NUMBER(detected_hz); ADD_NUMBER(band_step50); ADD_NUMBER(band_step60);
    ADD_NUMBER(max_bands50); ADD_NUMBER(max_bands60);
    ADD_BOOL(banding_enabled); ADD_BOOL(night_mode); ADD_BOOL(wb_manual);
    ADD_NUMBER(exposure_lines);
#undef ADD_BOOL
#undef ADD_NUMBER
    cJSON_AddStringToObject(sample, "sensor_name", telemetry.sensor_pid == 0x3660 ? "OV3660" : "unknown");
    if (telemetry.available) {
        for (unsigned i = 0; i < ESPCLAW_CAMERA_REGISTER_COUNT; ++i) {
            char key[12];
            snprintf(key, sizeof(key), "0x%04x", (unsigned)espclaw_camera_register_addresses[i]);
            cJSON_AddNumberToObject(registers, key, telemetry.registers[i]);
        }
    }
    return send_json(req, "200 OK", object);
}

static esp_err_t camera_settings_get_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_FAIL;
    return camera_settings_response(req); /* Cached values only; no camera init. */
}

static bool json_integer(const cJSON *item, int *value)
{
    if (!cJSON_IsNumber(item) || item->valuedouble != (double)item->valueint) return false;
    *value = item->valueint;
    return true;
}

static esp_err_t camera_settings_post_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_FAIL;
    char *body = read_body(req, 256U);
    if (!body) return ESP_FAIL;
    cJSON *object = http_json_has_nul_escape(body) ? NULL : cJSON_ParseWithOpts(body, NULL, true);
    free(body);
    const cJSON *flicker = cJSON_GetObjectItemCaseSensitive(object, "flicker_hz");
    const cJSON *wb = cJSON_GetObjectItemCaseSensitive(object, "wb_mode");
    espclaw_camera_settings_t settings = { .flicker_hz = -1, .wb_mode = -1 };
    bool valid = cJSON_IsObject(object) && cJSON_GetArraySize(object) == 5 &&
        json_integer(cJSON_GetObjectItemCaseSensitive(object, "fps"), &settings.fps) &&
        json_integer(cJSON_GetObjectItemCaseSensitive(object, "brightness"), &settings.brightness) &&
        json_integer(cJSON_GetObjectItemCaseSensitive(object, "saturation"), &settings.saturation) &&
        cJSON_IsString(flicker) && cJSON_IsString(wb);
    if (valid) {
        if (!strcmp(flicker->valuestring, "auto")) settings.flicker_hz = 0;
        else if (!strcmp(flicker->valuestring, "50")) settings.flicker_hz = 50;
        else if (!strcmp(flicker->valuestring, "60")) settings.flicker_hz = 60;
        if (!strcmp(wb->valuestring, "auto")) settings.wb_mode = 0;
        else if (!strcmp(wb->valuestring, "daylight")) settings.wb_mode = 1;
        else if (!strcmp(wb->valuestring, "office")) settings.wb_mode = 3;
        else if (!strcmp(wb->valuestring, "home")) settings.wb_mode = 4;
        valid = espclaw_camera_settings_valid(&settings);
    }
    cJSON_Delete(object);
    if (!valid) return send_error(req, "400 Bad Request", "Expected all five allowlisted camera settings");
    if (!job_lock()) return send_error(req, "503 Service Unavailable", "State is busy");
    job_expire();
    bool busy = s_job.pending || s_capture_active || s_stream_active;
    esp_err_t err = busy ? ESP_ERR_INVALID_STATE : espclaw_camera_set_settings(&settings);
    xSemaphoreGive(s_job_mutex);
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "Stop preview and wait until the device is idle");
    if (err != ESP_OK) return send_error(req, "503 Service Unavailable", "Unable to store camera settings");
    return camera_settings_response(req);
}

#define STREAM_BOUNDARY "espclawframe"
typedef struct {
    httpd_req_t *request;
    bool headers_sent;
    bool send_failed;
} stream_context_t;

static bool stream_should_stop(void *context)
{
    (void)context;
    if (!job_lock()) return true;
    bool stop = s_stream_stop;
    xSemaphoreGive(s_job_mutex);
    return stop;
}

static esp_err_t stream_send_frame(const uint8_t *jpeg, size_t jpeg_size, void *context)
{
    stream_context_t *stream = context;
    char part_header[128];
    int header_size = snprintf(part_header, sizeof(part_header),
        "--" STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
        (unsigned)jpeg_size);
    if (header_size <= 0 || (size_t)header_size >= sizeof(part_header)) return ESP_FAIL;
    stream->headers_sent = true;
    esp_err_t err = httpd_resp_send_chunk(stream->request, part_header, header_size);
    if (err == ESP_OK)
        err = httpd_resp_send_chunk(stream->request, (const char *)jpeg, jpeg_size);
    if (err == ESP_OK) err = httpd_resp_send_chunk(stream->request, "\r\n", 2);
    if (err != ESP_OK) stream->send_failed = true;
    return err;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_FAIL;
    if (!job_lock()) return send_error(req, "503 Service Unavailable", "State is busy");
    job_expire();
    bool busy = s_job.pending || s_capture_active || s_stream_active || camera_busy();
    if (!busy) {
        s_stream_active = true;
        s_stream_stop = false;
    }
    xSemaphoreGive(s_job_mutex);
    if (busy) return send_error(req, "409 Conflict", "Device is busy");

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=" STREAM_BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    wifi_ps_type_t previous_power_save = WIFI_PS_NONE;
    bool restore_power_save = esp_wifi_get_ps(&previous_power_save) == ESP_OK &&
        previous_power_save != WIFI_PS_NONE && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
    int socket_fd = httpd_req_to_sockfd(req);
    int previous_nodelay = 0;
    int enable_nodelay = 1;
    socklen_t option_size = sizeof(previous_nodelay);
    bool restore_nodelay = socket_fd >= 0 &&
        getsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &previous_nodelay, &option_size) == 0 &&
        previous_nodelay != enable_nodelay &&
        setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enable_nodelay, sizeof(enable_nodelay)) == 0;
    stream_context_t stream = { .request = req };
    esp_err_t err = espclaw_camera_stream_jpeg(stream_send_frame, stream_should_stop, &stream);
    if (restore_nodelay)
        setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &previous_nodelay, sizeof(previous_nodelay));
    if (restore_power_save && esp_wifi_set_ps(previous_power_save) != ESP_OK)
        ESP_LOGW(TAG, "Unable to restore Wi-Fi power-save policy after preview");
    release_camera_reservation(true);
    if (!stream.headers_sent && err != ESP_OK)
        return send_error(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "503 Service Unavailable",
                          err == ESP_ERR_INVALID_STATE ? "Camera is busy" : esp_err_to_name(err));
    if (stream.send_failed) return ESP_FAIL;
    /* Finish multipart and HTTP chunking after sensor teardown. Also close
     * gracefully on a sensor error when the client connection still works. */
    err = httpd_resp_send_chunk(req, "--" STREAM_BOUNDARY "--\r\n", HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}
#endif

static esp_err_t message_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_FAIL;
    char *body = read_body(req, HTTP_BODY_LIMIT);
    if (!body) return ESP_FAIL;
    /* cJSON strings cannot represent embedded NULs. Reject that escape rather
     * than silently accepting a truncated text or job ID. */
    bool embedded_nul = http_json_has_nul_escape(body);
    const char *end = NULL;
    cJSON *object = cJSON_ParseWithOpts(body, &end, true);
    cJSON *text = cJSON_GetObjectItemCaseSensitive(object, "text");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
    int64_t job_id = 0;
    if (embedded_nul || !cJSON_IsObject(object) || !cJSON_IsString(text) ||
        !cJSON_IsString(id) || !http_parse_job_id(id->valuestring, &job_id)) {
        cJSON_Delete(object); free(body);
        return send_error(req, "400 Bad Request", "Expected text and a positive decimal-string id (up to 18 digits)");
    }
    size_t text_length = strlen(text->valuestring);
    if (!text_length || text_length >= CHANNEL_RX_BUF_SIZE || text_length > 1023U ||
        http_utf8_prefix_length(text->valuestring, text_length) != text_length) {
        cJSON_Delete(object); free(body);
        return send_error(req, "400 Bad Request", "Text must be valid UTF-8, 1 to 1023 bytes");
    }
    inbound_msg_t message = { .source = MSG_SOURCE_HTTP, .chat_id = job_id };
    memcpy(message.text, text->valuestring, text_length + 1U);
    cJSON_Delete(object);
    free(body);
    if (!job_lock()) return send_error(req, "503 Service Unavailable", "State is busy");
    job_expire();
    if (s_job.pending || s_capture_active || s_stream_active || camera_busy()) {
        xSemaphoreGive(s_job_mutex);
        return send_error(req, "409 Conflict", "Device is busy");
    }
    if (job_id <= s_job.last_id) {
        xSemaphoreGive(s_job_mutex);
        return send_error(req, "409 Conflict", "Job id must increase; replayed ids are not accepted");
    }
    s_job.id = s_job.last_id = job_id;
    s_job.pending = true;
    s_job.started_us = esp_timer_get_time();
    s_job.finished_us = 0;
    s_job.reply[0] = '\0';
    esp_err_t err = message_bus_post_inbound(s_bus, &message, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        s_job.pending = false;
        s_job.id = 0;
        xSemaphoreGive(s_job_mutex);
        return send_error(req, "503 Service Unavailable", "Agent queue is full");
    }
    xSemaphoreGive(s_job_mutex);
    char id_text[24];
    snprintf(id_text, sizeof(id_text), "%" PRId64, job_id);
    object = cJSON_CreateObject();
    if (object) {
        cJSON_AddStringToObject(object, "id", id_text);
        cJSON_AddStringToObject(object, "status", "pending");
    }
    return send_json(req, "202 Accepted", object);
}

static esp_err_t result_handler(httpd_req_t *req)
{
    if (!authenticated(req)) return ESP_FAIL;
    char query[64], id_text[24];
    int64_t id = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id_text, sizeof(id_text)) != ESP_OK ||
        !http_parse_job_id(id_text, &id))
        return send_error(req, "400 Bad Request", "A decimal-string id query parameter is required");
    if (!job_lock()) return send_error(req, "503 Service Unavailable", "State is busy");
    job_expire();
    if (s_job.id != id) {
        xSemaphoreGive(s_job_mutex);
        return send_error(req, "404 Not Found", "Unknown job id");
    }
    bool pending = s_job.pending;
    char reply[CHANNEL_TX_BUF_SIZE];
    memcpy(reply, s_job.reply, sizeof(reply));
    xSemaphoreGive(s_job_mutex);
    cJSON *object = cJSON_CreateObject();
    if (object) {
        cJSON_AddStringToObject(object, "id", id_text);
        cJSON_AddStringToObject(object, "status", pending ? "pending" : "done");
        if (!pending) cJSON_AddStringToObject(object, "reply", reply);
    }
    return send_json(req, "200 OK", object);
}

void http_post(const char *text, int64_t job_id)
{
    if (!text || !job_lock()) return;
    job_expire();
    if (s_job.pending && s_job.id == job_id) {
        size_t bytes = strnlen(text, sizeof(s_job.reply) - 1U);
        bytes = http_utf8_prefix_length(text, bytes);
        memcpy(s_job.reply, text, bytes);
        s_job.reply[bytes] = '\0';
        s_job.pending = false;
        s_job.finished_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_job_mutex);
}

static bool http_is_available(void)
{
    return true; /* Listener can start before Wi-Fi obtains an address. */
}

static esp_err_t http_start(message_bus_t *bus)
{
    if (s_server) return ESP_OK;
    if (!nvs_mgr_get_str(NVS_KEY_LLM_API_KEY, s_token, sizeof(s_token)) || strlen(s_token) < 32U) {
        ESP_LOGW(TAG, "HTTP disabled: provision an NVS bridge token of at least 32 characters");
        return ESP_ERR_NOT_FOUND;
    }
    s_bus = bus;
    s_job_mutex = xSemaphoreCreateMutex();
    if (!s_job_mutex) return ESP_ERR_NO_MEM;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 12288;
    config.max_uri_handlers = 7;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err == ESP_OK) {
        const httpd_uri_t handlers[] = {
            { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
            { .uri = "/api/capture", .method = HTTP_POST, .handler = capture_handler },
            { .uri = "/api/message", .method = HTTP_POST, .handler = message_handler },
            { .uri = "/api/result", .method = HTTP_GET, .handler = result_handler },
            { .uri = "/api/stream/stop", .method = HTTP_POST, .handler = stream_stop_handler },
#ifdef CONFIG_ESPCLAW_CAMERA_SENSE
            { .uri = "/api/camera/settings", .method = HTTP_GET, .handler = camera_settings_get_handler },
            { .uri = "/api/camera/settings", .method = HTTP_POST, .handler = camera_settings_post_handler },
#endif
        };
        for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
            err = httpd_register_uri_handler(s_server, &handlers[i]);
            if (err != ESP_OK) break;
        }
    }
#ifdef CONFIG_ESPCLAW_CAMERA_SENSE
    if (err == ESP_OK) {
        httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
        stream_config.server_port = 81;
        stream_config.ctrl_port = config.ctrl_port + 1;
        stream_config.stack_size = 8192;
        stream_config.max_uri_handlers = 1;
        stream_config.max_open_sockets = 2;
        stream_config.recv_wait_timeout = 2;
        stream_config.send_wait_timeout = 2;
        err = httpd_start(&s_stream_server, &stream_config);
        if (err == ESP_OK) {
            const httpd_uri_t stream_uri = {
                .uri = "/api/stream", .method = HTTP_GET, .handler = stream_handler,
            };
            err = httpd_register_uri_handler(s_stream_server, &stream_uri);
        }
    }
#endif
    if (err != ESP_OK) {
#ifdef CONFIG_ESPCLAW_CAMERA_SENSE
        if (s_stream_server) httpd_stop(s_stream_server);
        s_stream_server = NULL;
#endif
        if (s_server) httpd_stop(s_server);
        s_server = NULL;
        vSemaphoreDelete(s_job_mutex);
        s_job_mutex = NULL;
        return err;
    }
    ESP_LOGI(TAG, "Authenticated local HTTP API listening on port 80");
#ifdef CONFIG_ESPCLAW_CAMERA_SENSE
    ESP_LOGI(TAG, "Authenticated camera stream listening on port 81");
#endif
    return ESP_OK;
}

const channel_ops_t http_channel_ops = {
    .name = "http",
    .start = http_start,
    .is_available = http_is_available,
};
