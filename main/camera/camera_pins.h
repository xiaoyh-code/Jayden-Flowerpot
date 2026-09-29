#ifndef ESPCLAW_CAMERA_PINS_H
#define ESPCLAW_CAMERA_PINS_H

#include <stdbool.h>

/* Seeed Studio XIAO ESP32S3 Sense expansion board pin map:
 * https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/
 * This mapping is specific to Sense, not generic ESP32-S3 camera boards. */
#define SENSE_CAMERA_XCLK  10
#define SENSE_CAMERA_SDA   40
#define SENSE_CAMERA_SCL   39
#define SENSE_CAMERA_D0    15
#define SENSE_CAMERA_D1    17
#define SENSE_CAMERA_D2    18
#define SENSE_CAMERA_D3    16
#define SENSE_CAMERA_D4    14
#define SENSE_CAMERA_D5    12
#define SENSE_CAMERA_D6    11
#define SENSE_CAMERA_D7    48
#define SENSE_CAMERA_VSYNC 38
#define SENSE_CAMERA_HREF  47
#define SENSE_CAMERA_PCLK  13

/* Keep these pins reserved even between captures: the camera remains
 * physically connected when its driver is stopped. */
static inline bool espclaw_camera_pin_reserved(int pin)
{
    switch (pin) {
    case SENSE_CAMERA_XCLK:
    case SENSE_CAMERA_SDA:
    case SENSE_CAMERA_SCL:
    case SENSE_CAMERA_D0:
    case SENSE_CAMERA_D1:
    case SENSE_CAMERA_D2:
    case SENSE_CAMERA_D3:
    case SENSE_CAMERA_D4:
    case SENSE_CAMERA_D5:
    case SENSE_CAMERA_D6:
    case SENSE_CAMERA_D7:
    case SENSE_CAMERA_VSYNC:
    case SENSE_CAMERA_HREF:
    case SENSE_CAMERA_PCLK:
        return true;
    default:
        return false;
    }
}

#endif
