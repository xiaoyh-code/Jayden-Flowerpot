/*
 * ESPClaw - provider/provider_openai.c
 * OpenAI-compatible Chat Completions, including local multimodal requests.
 */
#include "provider.h"
#include "provider_openai_json.h"
#include "platform.h"
#include "config.h"
#include "tool/tool_registry.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "openai";

#if ESPCLAW_HAS_PSRAM
#define OPENAI_MAX_REQUEST_BYTES (1024 * 1024)
#else
#define OPENAI_MAX_REQUEST_BYTES LLM_REQUEST_BUF_SIZE
#endif
/* Local vision prefill and dense-model inference can exceed the API default. */
#define OPENAI_HTTP_TIMEOUT_MS 180000

typedef struct {
    char *buf;
    size_t buf_sz;
    size_t written;
    bool overflow;
} http_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t remaining = ctx->buf_sz - ctx->written - 1;
        size_t to_copy = (size_t)evt->data_len;
        if (to_copy > remaining) {
            to_copy = remaining;
            ctx->overflow = true;
        }
        if (to_copy > 0) {
            memcpy(ctx->buf + ctx->written, evt->data, to_copy);
            ctx->written += to_copy;
            ctx->buf[ctx->written] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t json_status_to_error(openai_json_status_t status)
{
    switch (status) {
        case OPENAI_JSON_OK: return ESP_OK;
        case OPENAI_JSON_NO_MEMORY: return ESP_ERR_NO_MEM;
        case OPENAI_JSON_TOO_LARGE: return ESP_ERR_INVALID_SIZE;
        default: return ESP_ERR_INVALID_RESPONSE;
    }
}

static char s_api_key[LLM_API_KEY_BUF_SIZE];
static char s_model[64];
static char s_base_url[128];
static bool s_bearer_auth;

static esp_err_t openai_init(const char *api_key, const char *model,
                             const char *base_url)
{
    const char *url = base_url && base_url[0] ? base_url : LLM_API_URL_OPENAI;
    if (!api_key || !model || !model[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(api_key) >= sizeof(s_api_key) || strlen(model) >= sizeof(s_model) ||
        strlen(url) >= sizeof(s_base_url)) return ESP_ERR_INVALID_SIZE;
    memcpy(s_api_key, api_key, strlen(api_key) + 1);
    memcpy(s_model, model, strlen(model) + 1);
    memcpy(s_base_url, url, strlen(url) + 1);
    s_bearer_auth = s_api_key[0] != '\0';
    return ESP_OK;
}

static esp_err_t openai_complete(
    const char *system_prompt,
    const char *messages_json,
    const char *tools_json,
    char *response_buf,
    size_t response_sz)
{
    if (!response_buf || response_sz == 0) return ESP_ERR_INVALID_ARG;
    response_buf[0] = '\0';

    /* The agent passes Anthropic schemas; regenerate only when tools are wanted.
     * /look passes NULL and must never advertise GPIO or other action tools. */
    char *oai_tools = NULL;
    if (tools_json && tools_json[0] && strcmp(tools_json, "[]") != 0) {
        oai_tools = malloc(LLM_REQUEST_BUF_SIZE);
        if (!oai_tools) return ESP_ERR_NO_MEM;
        if (tool_registry_build_tools_json_openai(oai_tools, LLM_REQUEST_BUF_SIZE) < 0) {
            free(oai_tools);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    char *body = NULL;
    openai_json_status_t json_status = openai_json_build_request(
        s_model, LLM_MAX_TOKENS, system_prompt, messages_json, oai_tools,
        OPENAI_MAX_REQUEST_BYTES, &body);
    free(oai_tools);
    if (json_status != OPENAI_JSON_OK) return json_status_to_error(json_status);
    size_t body_len = strlen(body);
    if (body_len > INT_MAX) { cJSON_free(body); return ESP_ERR_INVALID_SIZE; }
    /* Do not copy prompts or camera image data into serial logs. */
    ESP_LOGI(TAG, "Request body: %u bytes", (unsigned)body_len);

    if (!espclaw_tls_lock(pdMS_TO_TICKS(OPENAI_HTTP_TIMEOUT_MS))) {
        cJSON_free(body);
        return ESP_ERR_TIMEOUT;
    }
    char *resp = malloc(LLM_RESPONSE_BUF_SIZE);
    if (!resp) {
        espclaw_tls_unlock();
        cJSON_free(body);
        return ESP_ERR_NO_MEM;
    }
    resp[0] = '\0';
    http_ctx_t ctx = { .buf = resp, .buf_sz = LLM_RESPONSE_BUF_SIZE };
    esp_http_client_config_t cfg = {
        .url = s_base_url, /* Full endpoint URL, including /v1/chat/completions. */
        .method = HTTP_METHOD_POST,
        .timeout_ms = OPENAI_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        espclaw_tls_unlock();
        cJSON_free(body);
        free(resp);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "content-type", "application/json; charset=utf-8");
    if (s_bearer_auth) {
        char auth_val[LLM_API_KEY_BUF_SIZE + 8];
        snprintf(auth_val, sizeof(auth_val), "Bearer %s", s_api_key);
        esp_http_client_set_header(client, "authorization", auth_val);
    }
    esp_http_client_set_post_field(client, body, (int)body_len);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    espclaw_tls_unlock();
    cJSON_free(body);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Request failed: %s, HTTP %d", esp_err_to_name(err), status);
        free(resp);
        return err != ESP_OK ? err : ESP_FAIL;
    }
    if (ctx.overflow) {
        ESP_LOGE(TAG, "Response exceeds %d bytes", LLM_RESPONSE_BUF_SIZE - 1);
        free(resp);
        return ESP_ERR_INVALID_SIZE;
    }
    json_status = openai_json_parse_reply(resp, response_buf, response_sz);
    free(resp);
    if (json_status != OPENAI_JSON_OK) {
        ESP_LOGE(TAG, "Invalid or oversized Chat Completions response (%d)", json_status);
    }
    return json_status_to_error(json_status);
}

const provider_ops_t openai_provider = {
    .name = "openai", .init = openai_init, .complete = openai_complete, .deinit = NULL,
};
const provider_ops_t openrouter_provider = {
    .name = "openrouter", .init = openai_init, .complete = openai_complete, .deinit = NULL,
};
const provider_ops_t ollama_provider = {
    .name = "ollama", .init = openai_init, .complete = openai_complete, .deinit = NULL,
};
