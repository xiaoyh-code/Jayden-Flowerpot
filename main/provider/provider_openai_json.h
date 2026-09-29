/* OpenAI wire-format helpers, kept independent of ESP-IDF for native fixtures. */
#ifndef PROVIDER_OPENAI_JSON_H
#define PROVIDER_OPENAI_JSON_H

#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef enum {
    OPENAI_JSON_OK = 0,
    OPENAI_JSON_INVALID,
    OPENAI_JSON_NO_MEMORY,
    OPENAI_JSON_TOO_LARGE,
} openai_json_status_t;

/* Match the existing agent/session storage; never silently truncate a tool. */
#define OPENAI_TOOL_ID_MAX_BYTES     63
#define OPENAI_TOOL_NAME_MAX_BYTES   31
#define OPENAI_TOOL_INPUT_MAX_BYTES  255

static bool openai_json_safe_identifier(const char *value, size_t max_bytes)
{
    if (!value || !value[0] || strlen(value) > max_bytes) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        /* session.c interpolates IDs and names into JSON without escaping. */
        if (*p < 0x21 || *p > 0x7e || *p == '"' || *p == '\\') return false;
    }
    return true;
}

static openai_json_status_t openai_json_parse_reply(
    const char *json, char *out, size_t out_sz)
{
    if (!json || !out || out_sz == 0) return OPENAI_JSON_INVALID;
    out[0] = '\0';
    openai_json_status_t status = OPENAI_JSON_INVALID;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    cJSON *input = NULL;
    cJSON *normalized = NULL;
    char *encoded = NULL;
    char *input_text = NULL;
    if (!root) return status;

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *choice = cJSON_GetArrayItem(choices, 0);
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    if (!cJSON_IsArray(choices) || !cJSON_IsObject(message)) goto done;

    const cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
    if (calls && !cJSON_IsNull(calls) && !cJSON_IsArray(calls)) goto done;
    if (cJSON_IsArray(calls) && cJSON_GetArraySize(calls) > 0) {
        /* The current agent executes one tool per round. Do not drop others. */
        if (cJSON_GetArraySize(calls) != 1) goto done;
        const cJSON *call = cJSON_GetArrayItem(calls, 0);
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(call, "type");
        const cJSON *function = cJSON_GetObjectItemCaseSensitive(call, "function");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(function, "name");
        const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(function, "arguments");
        if (!cJSON_IsString(id) || !cJSON_IsString(name) ||
            !cJSON_IsString(arguments) || !cJSON_IsString(type) ||
            strcmp(type->valuestring, "function") != 0 ||
            !openai_json_safe_identifier(id->valuestring, OPENAI_TOOL_ID_MAX_BYTES) ||
            !openai_json_safe_identifier(name->valuestring, OPENAI_TOOL_NAME_MAX_BYTES))
            goto done;

        input = cJSON_ParseWithOpts(arguments->valuestring, NULL, true);
        if (!cJSON_IsObject(input)) goto done;
        input_text = cJSON_PrintUnformatted(input);
        if (!input_text) { status = OPENAI_JSON_NO_MEMORY; goto done; }
        if (strlen(input_text) > OPENAI_TOOL_INPUT_MAX_BYTES) {
            status = OPENAI_JSON_TOO_LARGE;
            goto done;
        }

        normalized = cJSON_CreateObject();
        if (!normalized ||
            !cJSON_AddStringToObject(normalized, "stop_reason", "tool_use") ||
            !cJSON_AddStringToObject(normalized, "id", id->valuestring) ||
            !cJSON_AddStringToObject(normalized, "name", name->valuestring)) {
            status = OPENAI_JSON_NO_MEMORY;
            goto done;
        }
        if (!cJSON_AddItemToObject(normalized, "input", input)) {
            status = OPENAI_JSON_NO_MEMORY;
            goto done;
        }
        input = NULL; /* owned by normalized */
        encoded = cJSON_PrintUnformatted(normalized);
        if (!encoded) { status = OPENAI_JSON_NO_MEMORY; goto done; }
        if (strlen(encoded) >= out_sz) { status = OPENAI_JSON_TOO_LARGE; goto done; }
        memcpy(out, encoded, strlen(encoded) + 1);
    } else {
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
        if (!cJSON_IsString(content)) goto done;
        if (strlen(content->valuestring) >= out_sz) {
            status = OPENAI_JSON_TOO_LARGE;
            goto done;
        }
        memcpy(out, content->valuestring, strlen(content->valuestring) + 1);
    }
    status = OPENAI_JSON_OK;

done:
    cJSON_free(input_text);
    cJSON_free(encoded);
    cJSON_Delete(normalized);
    cJSON_Delete(input);
    cJSON_Delete(root);
    return status;
}

/* Returned request is allocated by cJSON; the caller must cJSON_free it. */
static openai_json_status_t openai_json_build_request(
    const char *model, int max_tokens, const char *system_prompt,
    const char *messages_json, const char *tools_json,
    size_t max_bytes, char **out)
{
    if (!out) return OPENAI_JSON_INVALID;
    *out = NULL;
    if (!model || !model[0] || max_tokens <= 0 || !messages_json || max_bytes == 0)
        return OPENAI_JSON_INVALID;

    size_t input_bytes = strlen(messages_json);
    if (input_bytes > max_bytes) return OPENAI_JSON_TOO_LARGE;
    const char *inputs[] = {model, system_prompt, tools_json};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        size_t bytes = inputs[i] ? strlen(inputs[i]) : 0;
        if (bytes > max_bytes - input_bytes) return OPENAI_JSON_TOO_LARGE;
        input_bytes += bytes;
    }

    openai_json_status_t status = OPENAI_JSON_INVALID;
    cJSON *request = cJSON_CreateObject();
    cJSON *messages = cJSON_ParseWithOpts(messages_json, NULL, true);
    cJSON *tools = NULL;
    cJSON *system = NULL;
    char *body = NULL;
    if (!request) { status = OPENAI_JSON_NO_MEMORY; goto done; }
    if (!cJSON_IsArray(messages) || cJSON_GetArraySize(messages) == 0) goto done;
    const cJSON *message = NULL;
    cJSON_ArrayForEach(message, messages) {
        if (!cJSON_IsObject(message)) goto done;
    }
    if (!cJSON_AddStringToObject(request, "model", model) ||
        !cJSON_AddNumberToObject(request, "max_tokens", max_tokens)) {
        status = OPENAI_JSON_NO_MEMORY;
        goto done;
    }
    if (system_prompt && system_prompt[0]) {
        system = cJSON_CreateObject();
        if (!system ||
            !cJSON_AddStringToObject(system, "role", "system") ||
            !cJSON_AddStringToObject(system, "content", system_prompt) ||
            !cJSON_InsertItemInArray(messages, 0, system)) {
            status = OPENAI_JSON_NO_MEMORY;
            goto done;
        }
        system = NULL; /* owned by messages */
    }
    if (!cJSON_AddItemToObject(request, "messages", messages)) {
        status = OPENAI_JSON_NO_MEMORY;
        goto done;
    }
    messages = NULL; /* owned by request */

    if (tools_json && tools_json[0]) {
        tools = cJSON_ParseWithOpts(tools_json, NULL, true);
        if (!cJSON_IsArray(tools)) goto done;
        if (cJSON_GetArraySize(tools) > 0) {
            if (!cJSON_AddItemToObject(request, "tools", tools)) {
                status = OPENAI_JSON_NO_MEMORY;
                goto done;
            }
            tools = NULL; /* owned by request */
            /* The firmware agent's history supports one tool call per round. */
            if (!cJSON_AddBoolToObject(request, "parallel_tool_calls", false)) {
                status = OPENAI_JSON_NO_MEMORY;
                goto done;
            }
        }
    }
    body = cJSON_PrintUnformatted(request);
    if (!body) { status = OPENAI_JSON_NO_MEMORY; goto done; }
    if (strlen(body) > max_bytes) { status = OPENAI_JSON_TOO_LARGE; goto done; }
    *out = body;
    body = NULL;
    status = OPENAI_JSON_OK;

done:
    cJSON_free(body);
    cJSON_Delete(system);
    cJSON_Delete(tools);
    cJSON_Delete(messages);
    cJSON_Delete(request);
    return status;
}

#endif /* PROVIDER_OPENAI_JSON_H */
