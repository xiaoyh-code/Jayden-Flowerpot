#ifndef ESPCLAW_CHANNEL_HTTP_H
#define ESPCLAW_CHANNEL_HTTP_H

#include <stdint.h>

/* Called by the existing outbound dispatcher. Stale/expired IDs are ignored. */
void http_post(const char *text, int64_t job_id);

#endif
