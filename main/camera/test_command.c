/* Standalone host regression test; not part of the firmware build.
 * cc -std=c11 -Wall -Wextra -Werror main/camera/test_command.c -o /tmp/camera-command-test
 */
#include "camera_command.h"
#include "camera_pins.h"
#include <assert.h>

int main(void)
{
    assert(strcmp(espclaw_camera_command_question("/look", true), "") == 0);
    assert(strcmp(espclaw_camera_command_question("/look  What is this?", true),
                  "What is this?") == 0);
    assert(strcmp(espclaw_camera_command_question("/look\t讀出文字", true),
                  "讀出文字") == 0);
    assert(espclaw_camera_command_question("/looking", true) == NULL);
    assert(espclaw_camera_command_question("please /look", true) == NULL);
    assert(espclaw_camera_command_question(" /look", true) == NULL);
    assert(espclaw_camera_command_question("/look", false) == NULL);
    assert(espclaw_camera_command_question("/look take a photo", false) == NULL);
    assert(espclaw_camera_command_question("", true) == NULL);
    assert(espclaw_camera_command_question(NULL, true) == NULL);
    const int reserved[] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 38, 39, 40, 47, 48};
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
        assert(espclaw_camera_pin_reserved(reserved[i]));
    assert(!espclaw_camera_pin_reserved(2));
    assert(!espclaw_camera_pin_reserved(21));
    return 0;
}
