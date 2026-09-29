#ifndef ESPCLAW_HTTP_PROTOCOL_H
#define ESPCLAW_HTTP_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Canonical positive decimal job IDs fit the agent's signed chat_id field.
 * Eighteen digits also leave ample room below INT64_MAX. */
static inline bool http_parse_job_id(const char *text, int64_t *id)
{
    if (!text || !id || text[0] < '1' || text[0] > '9') return false;
    size_t length = strlen(text);
    if (length > 18U) return false;
    int64_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (text[i] - '0');
    }
    *id = value;
    return true;
}

/* Length is public; equal-length secret comparisons never short circuit. */
static inline bool http_token_equal(const char *candidate, const char *expected)
{
    size_t length = strlen(expected);
    if (strlen(candidate) != length) return false;
    volatile unsigned char difference = 0;
    for (size_t i = 0; i < length; ++i)
        difference |= (unsigned char)candidate[i] ^ (unsigned char)expected[i];
    return difference == 0;
}

/* cJSON stores strings as NUL-terminated C strings. Reject a decoded NUL,
 * while allowing a literal escaped backslash followed by "u0000". */
static inline bool http_json_has_nul_escape(const char *json)
{
    for (const char *p = json; *p; ++p) {
        if (*p != '\\') continue;
        ++p;
        if (!*p) break;
        if (*p == 'u' && strncmp(p + 1, "0000", 4U) == 0) return true;
    }
    return false;
}

/* Reject malformed, overlong, surrogate and out-of-range UTF-8. */
static inline size_t http_utf8_prefix_length(const char *text, size_t length)
{
    size_t i = 0;
    while (i < length) {
        unsigned char lead = (unsigned char)text[i];
        if (lead < 0x80U) { ++i; continue; }
        unsigned count;
        uint32_t codepoint, minimum;
        if (lead >= 0xc2U && lead <= 0xdfU) {
            count = 2; codepoint = lead & 0x1fU; minimum = 0x80U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            count = 3; codepoint = lead & 0x0fU; minimum = 0x800U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            count = 4; codepoint = lead & 0x07U; minimum = 0x10000U;
        } else {
            return i;
        }
        if (i + count > length) return i;
        for (unsigned j = 1; j < count; ++j) {
            unsigned char next = (unsigned char)text[i + j];
            if ((next & 0xc0U) != 0x80U) return i;
            codepoint = (codepoint << 6) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return i;
        i += count;
    }
    return i;
}

#endif
