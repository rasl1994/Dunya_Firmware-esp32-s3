#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t strength;
    bool pressed;
} touch_gt911_event_t;

typedef void (*touch_gt911_event_cb_t)(
    const touch_gt911_event_t *event,
    void *user_ctx
);

typedef struct {
    uint16_t x_max;
    uint16_t y_max;

    bool swap_xy;
    bool mirror_x;
    bool mirror_y;

    uint32_t poll_period_ms;

    touch_gt911_event_cb_t event_cb;
    void *user_ctx;
} touch_gt911_config_t;

#define TOUCH_GT911_CONFIG_DEFAULT() { \
    .x_max = 480,                      \
    .y_max = 480,                      \
    .swap_xy = false,                  \
    .mirror_x = false,                 \
    .mirror_y = false,                 \
    .poll_period_ms = 10,              \
    .event_cb = NULL,                  \
    .user_ctx = NULL,                  \
}

/**
 * Initialize GT911 on the shared board I2C bus.
 *
 * The GT911 reset pin is controlled through TCA9554 P0.
 * The interrupt pin is configured as a normal GPIO input.
 */
esp_err_t touch_gt911_init(const touch_gt911_config_t *config);

/** Start the polling task. Safe to call once after init. */
esp_err_t touch_gt911_start(void);

/** Stop and delete the polling task. */
esp_err_t touch_gt911_stop(void);

/** Read one sample synchronously. */
esp_err_t touch_gt911_read(touch_gt911_event_t *event);

/** Returns true after successful initialization. */
bool touch_gt911_is_initialized(void);

#ifdef __cplusplus
}
#endif
