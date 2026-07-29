#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVICE_SCREEN_EVENT_BACK = 0,
    SERVICE_SCREEN_EVENT_LOAD_CHANGED,
    SERVICE_SCREEN_EVENT_LOAD_ENABLED_CHANGED,
} service_screen_event_t;

typedef struct {
    bool load_enabled;
    uint8_t load_percent;

    float voltage_v;
    float current_a;
    float power_w;
    bool measurement_valid;

    float temperature_c;
    bool temperature_valid;
} service_screen_model_t;

typedef void (*service_screen_event_cb_t)(
    service_screen_event_t event,
    const service_screen_model_t *model,
    void *user_ctx
);

typedef struct {
    service_screen_event_cb_t event_cb;
    void *user_ctx;
} service_screen_config_t;

#define SERVICE_SCREEN_CONFIG_DEFAULT() { \
    .event_cb = NULL,                    \
    .user_ctx = NULL,                    \
}

esp_err_t service_screen_init(
    const service_screen_config_t *config,
    const service_screen_model_t *initial_model
);

esp_err_t service_screen_show(void);
esp_err_t service_screen_hide(void);
bool service_screen_is_visible(void);

void service_screen_set_load_percent(uint8_t percent);
void service_screen_set_load_enabled(bool enabled);

void service_screen_set_measurements(
    bool valid,
    float voltage_v,
    float current_a,
    float power_w
);

void service_screen_set_temperature(
    bool valid,
    float temperature_c
);

service_screen_model_t service_screen_get_model(void);

esp_err_t service_screen_handle_touch(
    uint16_t x,
    uint16_t y,
    bool pressed
);

#ifdef __cplusplus
}
#endif
