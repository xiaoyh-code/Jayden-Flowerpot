#ifndef ESPCLAW_CAMERA_TUNING_H
#define ESPCLAW_CAMERA_TUNING_H

#include "camera_capture.h"

static inline bool espclaw_camera_settings_valid(const espclaw_camera_settings_t *s)
{
    return s && (s->fps == 10 || s->fps == 15 || s->fps == 20 || s->fps == 25) &&
        (s->flicker_hz == 0 || s->flicker_hz == 50 || s->flicker_hz == 60) &&
        (s->wb_mode == 0 || s->wb_mode == 1 || s->wb_mode == 3 || s->wb_mode == 4) &&
        s->brightness >= -2 && s->brightness <= 2 &&
        s->saturation >= -2 && s->saturation <= 2;
}

/* OV3660 datasheet v1.3, figure 2-7 and tables 2-9/7-2, cross-checked with
 * pinned esp32-camera sensors/ov3660.c calc_sysclk. No PLL values are changed.
 * Denominators are represented as twice their value to preserve /1.5 and /2.5. */
static inline uint32_t espclaw_ov3660_sysclk(uint32_t xclk, int r303a, int r303b,
                                             int r303c, int r303d, int r3108)
{
    if (r303a < 0 || r303b < 0 || r303c < 0 || r303d < 0 || r3108 < 0) return 0;
    /* Datasheet root control is bit2. The pinned driver's optional root_2x
     * setter writes bit6 instead; reject that undocumented combination.
     * Our unchanged VGA clock uses0x30 (both bits clear). */
    if (r303d & 0xc8) return 0;
    static const unsigned pre2[] = {2, 3, 4, 6};
    static const unsigned seld2[] = {2, 2, 4, 5};
    uint64_t pll = xclk;
    if (!(r303a & 0x80)) {
        unsigned sys_div = r303c & 0x0f;
        if (!sys_div) sys_div = 1;
        pll = (uint64_t)xclk * (unsigned)(r303b & 0x1f) *
            ((r303d & 0x04) ? 2U : 1U) * 4U /
            (pre2[(r303d >> 4) & 3] * sys_div * seld2[r303d & 3]);
    }
    return (uint32_t)(pll / (1U << (r3108 & 3)));
}

/* Datasheet sections 3.4.3/3.4.4: one band is 10ms (50Hz) or 8.333ms
 * (60Hz) in row periods. The printed 60Hz formula has a denominator typo;
 * the prose and worked example correctly use 120. Whole-frame band limits
 * are floored, matching that example and never exceeding the frame period. */
static inline bool espclaw_ov3660_banding(uint32_t sysclk, unsigned hts, unsigned vts,
                                           unsigned *step50, unsigned *step60,
                                           unsigned *max50, unsigned *max60)
{
    if (!sysclk || !hts || !vts) return false;
    *step50 = (unsigned)(((uint64_t)sysclk + (uint64_t)hts * 50U) / ((uint64_t)hts * 100U));
    *step60 = (unsigned)(((uint64_t)sysclk + (uint64_t)hts * 60U) / ((uint64_t)hts * 120U));
    if (!*step50 || !*step60 || *step50 > 1023U || *step60 > 1023U) return false;
    *max50 = vts / *step50;
    *max60 = vts / *step60;
    return *max50 > 0 && *max60 > 0 && *max50 <= 63U && *max60 <= 63U;
}

#endif
