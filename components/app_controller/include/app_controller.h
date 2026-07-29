#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_SCREEN_NONE = 0,
    APP_SCREEN_HOME,
    APP_SCREEN_SERVICE,
} app_screen_t;

typedef struct {
    bool start_in_service_screen;

    uint16_t initial_target_temperature;
    uint16_t initial_current_temperature;
    uint8_t initial_battery_percent;

    uint8_t heater_maximum_percent;
    uint32_t heater_pwm_frequency_hz;

    float heater_max_current_a;
    float heater_max_power_w;
    float heater_min_bus_voltage_v;

    uint32_t ina226_poll_period_ms;
} app_controller_config_t;

#define APP_CONTROLLER_CONFIG_DEFAULT() { \
    .start_in_service_screen = false,     \
    .initial_target_temperature = 99,     \
    .initial_current_temperature = 25,    \
    .initial_battery_percent = 0,         \
    .heater_maximum_percent = 100,         \
    .heater_pwm_frequency_hz = 16000,     \
    .heater_max_current_a = 8.0f,         \
    .heater_max_power_w = 160.0f,          \
    .heater_min_bus_voltage_v = 9.0f,     \
    .ina226_poll_period_ms = 100,         \
}

esp_err_t app_controller_init(
    const app_controller_config_t *config
);

esp_err_t app_controller_show_home(void);
esp_err_t app_controller_show_service(void);

app_screen_t app_controller_get_screen(void);

#ifdef __cplusplus
}
#endif
