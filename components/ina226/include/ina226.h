#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float bus_voltage_v;
    float shunt_voltage_mv;

    /*
     * Raw current calculated directly from VSHUNT / RSHUNT,
     * before applying current_correction_factor.
     */
    float current_raw_a;

    /*
     * Diagnostic value obtained from the INA226 CURRENT register.
     */
    float current_register_a;

    /*
     * Final corrected values used by the application.
     */
    float current_a;
    float power_w;

    /*
     * Diagnostic hardware POWER-register value.
     */
    float power_register_w;

    uint32_t sample_number;
    bool valid;
} ina226_measurement_t;

typedef void (*ina226_measurement_cb_t)(
    const ina226_measurement_t *measurement,
    void *user_ctx
);

typedef struct {
    uint8_t i2c_address;
    float shunt_resistance_ohm;
    float max_expected_current_a;

    /*
     * Final current:
     *
     *     current_a = current_raw_a * current_correction_factor
     *
     * Adjust only this coefficient during calibration tests.
     */
    float current_correction_factor;

    /*
     * Raw absolute current below this level is forced to zero.
     */
    float current_zero_threshold_a;

    uint32_t poll_period_ms;
    ina226_measurement_cb_t measurement_cb;
    void *user_ctx;
} ina226_config_t;

#define INA226_CONFIG_DEFAULT() {          \
    .i2c_address = 0x40,                   \
    .shunt_resistance_ohm = 0.01f,         \
    .max_expected_current_a = 10.0f,       \
    .current_correction_factor = 1.45f,    \
    .current_zero_threshold_a = 0.03f,     \
    .poll_period_ms = 100,                 \
    .measurement_cb = NULL,                \
    .user_ctx = NULL,                      \
}

esp_err_t ina226_init(const ina226_config_t *config);
esp_err_t ina226_start(void);
esp_err_t ina226_stop(void);

esp_err_t ina226_read(
    ina226_measurement_t *measurement
);

esp_err_t ina226_get_latest(
    ina226_measurement_t *measurement
);

/*
 * Perform one fast bus-voltage-only conversion.
 *
 * The function temporarily changes the INA226 conversion mode and restores
 * the normal continuous configuration before returning. The caller may pause
 * the PWM first when a clean source-voltage sample is required.
 */
esp_err_t ina226_read_bus_voltage_triggered(
    float *bus_voltage_v
);

bool ina226_is_initialized(void);
bool ina226_is_running(void);

#ifdef __cplusplus
}
#endif
