#ifndef ESPCLAW_CAMERA_COMMAND_H
#define ESPCLAW_CAMERA_COMMAND_H

#include <stdbool.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Only an explicit, whole /look command from the local serial channel may
 * trigger a capture. Model replies, cron jobs and other channels cannot. */
static inline const char *espclaw_camera_command_question(const char *text,
                                                         bool from_serial)
{
    if (!from_serial || !text || strncmp(text, "/look", 5) != 0)
        return NULL;
    const char *question = text + 5;
    if (*question && !isspace((unsigned char)*question))
        return NULL;
    while (isspace((unsigned char)*question))
        question++;
    return question;
}

#endif
