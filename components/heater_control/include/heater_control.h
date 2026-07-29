#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HEATER_FAULT_NONE         = 0,
    HEATER_FAULT_OVERCURRENT  = 1U << 0,
    HEATER_FAULT_OVERPOWER    = 1U << 1,
    HEATER_FAULT_UNDERVOLTAGE = 1U << 2,
    HEATER_FAULT_SENSOR       = 1U << 3,
    HEATER_FAULT_MANUAL_STOP  = 1U << 4,
} heater_fault_t;

typedef struct {
    bool initialized;

    bool enabled;
    uint8_t requested_percent;

    bool driver_enabled;
    uint8_t applied_percent;

    uint32_t fault_flags;

    float bus_voltage_v;
    float current_a;
    float power_w;
} heater_control_state_t;

typedef void (*heater_fault_cb_t)(
    uint32_t fault_flags,
    const heater_control_state_t *state,
    void *user_ctx
);

typedef struct {
    /*
     * HEATER_PWM is connected directly to an ESP32-S3 GPIO.
     */
    gpio_num_t pwm_gpio;

    /*
     * HEATER_EN is connected to TCA9554 P3.
     * This field stores the TCA9554 pin number, not an ESP32 GPIO.
     */
    uint8_t enable_iox_pin;
    bool enable_active_high;

    uint32_t pwm_frequency_hz;
    uint8_t maximum_percent;

    /*
     * Delay after HEATER_EN becomes active. This allows TLV61046
     * and the UCC27517 supply rail to start before PWM is applied.
     */
    uint32_t driver_startup_delay_ms;

    ledc_mode_t speed_mode;
    ledc_timer_t timer;
    ledc_channel_t channel;
    ledc_timer_bit_t duty_resolution;

    /*
     * Protection thresholds. A value <= 0 disables that check.
     * Configure these values for the real heater and power supply.
     */
    float max_current_a;
    float max_power_w;
    float min_bus_voltage_v;

    /*
     * Number of consecutive bad INA226 samples required before shutdown.
     */
    uint8_t fault_confirm_samples;

    heater_fault_cb_t fault_cb;
    void *user_ctx;
} heater_control_config_t;

#define HEATER_CONTROL_CONFIG_DEFAULT() { \
    .pwm_gpio = GPIO_NUM_NC,              \
    .enable_iox_pin = 3,                  \
    .enable_active_high = true,           \
    .pwm_frequency_hz = 16000,            \
    .maximum_percent = 100,               \
    .driver_startup_delay_ms = 20,        \
    .speed_mode = LEDC_LOW_SPEED_MODE,    \
    .timer = LEDC_TIMER_1,                \
    .channel = LEDC_CHANNEL_1,            \
    .duty_resolution = LEDC_TIMER_10_BIT, \
    .max_current_a = 0.0f,                \
    .max_power_w = 0.0f,                  \
    .min_bus_voltage_v = 0.0f,            \
    .fault_confirm_samples = 3,           \
    .fault_cb = NULL,                     \
    .user_ctx = NULL,                     \
}

esp_err_t heater_control_init(
    const heater_control_config_t *config
);

esp_err_t heater_control_set_enabled(bool enabled);
esp_err_t heater_control_set_power_percent(uint8_t percent);

esp_err_t heater_control_emergency_stop(uint32_t fault_flags);
esp_err_t heater_control_clear_faults(void);

esp_err_t heater_control_update_measurement(
    bool valid,
    float bus_voltage_v,
    float current_a,
    float power_w
);

heater_control_state_t heater_control_get_state(void);

#ifdef __cplusplus
}
#endif
