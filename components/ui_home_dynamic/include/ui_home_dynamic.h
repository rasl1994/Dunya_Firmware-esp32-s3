#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_HOME_EVENT_POWER_TOGGLED = 0,
    UI_HOME_EVENT_PLUS,
    UI_HOME_EVENT_MINUS,
    UI_HOME_EVENT_AI_TOGGLED,
} ui_home_event_t;

typedef struct {
    bool power_on;
    bool ai_enabled;
    bool running;
    bool paused;
    uint16_t target_temperature;
    uint16_t current_temperature;
    uint8_t battery_percent;
} ui_home_model_t;

typedef void (*ui_home_event_cb_t)(ui_home_event_t event,
                                   const ui_home_model_t *model,
                                   void *user_ctx);

typedef struct {
    const char *asset_base_path; /* Default: /spiffs/ui */
    ui_home_event_cb_t event_cb;
    void *user_ctx;
} ui_home_config_t;

#define UI_HOME_CONFIG_DEFAULT() { \
    .asset_base_path = "/spiffs/ui", \
    .event_cb = NULL, \
    .user_ctx = NULL, \
}

/** Load button assets and draw the complete home screen. SPIFFS must be mounted. */
esp_err_t ui_home_init(const ui_home_config_t *config,
                       const ui_home_model_t *initial_model);

/** Redraw the whole screen. Normally setters perform partial redraws. */
esp_err_t ui_home_redraw_all(void);

void ui_home_set_target_temperature(uint16_t celsius);
void ui_home_set_current_temperature(uint16_t celsius);
void ui_home_set_battery_percent(uint8_t percent);

/** Backward-compatible alias. New code should use ui_home_set_battery_percent(). */
static inline void ui_home_set_power_percent(uint8_t percent)
{
    ui_home_set_battery_percent(percent);
}

void ui_home_set_power_enabled(bool enabled);
void ui_home_set_ai_enabled(bool enabled);
void ui_home_set_running(bool running);
void ui_home_set_paused(bool paused);

ui_home_model_t ui_home_get_model(void);

/** Feed raw touch state. Coordinates are display coordinates, 0..479. */
esp_err_t ui_home_handle_touch(uint16_t x, uint16_t y, bool pressed);

#ifdef __cplusplus
}
#endif
