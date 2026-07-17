#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *base_path;      /* Example: /spiffs/intro */
    uint16_t frame_count;       /* 100 for LOAD_INTRO_00000..00099 */
    uint16_t frame_period_ms;   /* 33 = about 30 FPS */
    bool loop;
} intro_animation_config_t;

#define INTRO_ANIMATION_CONFIG_DEFAULT() { \
    .base_path = "/spiffs/intro",          \
    .frame_count = 100,                    \
    .frame_period_ms = 33,                 \
    .loop = false,                         \
}

esp_err_t intro_animation_mount_spiffs(void);
esp_err_t intro_animation_play(const intro_animation_config_t *config);
void intro_animation_request_stop(void);

#ifdef __cplusplus
}
#endif
