#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*max6675_temperature_cb_t)(
    bool valid,
    float temperature_c,
    void *user_ctx
);

/*
 * Initialize the MAX6675 interface.
 *
 * Hardware mapping:
 *   SO / MISO -> ESP32-S3 GPIO19
 *   SCK       -> ESP32-S3 GPIO20
 *   CS        -> TCA9554 P6, active low
 */
esp_err_t max6675_init(void);

/*
 * Start periodic temperature polling.
 * Recommended value for this project: 500 ms.
 */
esp_err_t max6675_start_task(uint32_t period_ms);

/*
 * Register a callback that receives each temperature sample.
 */
esp_err_t max6675_set_callback(
    max6675_temperature_cb_t callback,
    void *user_ctx
);

/*
 * Perform one synchronous temperature read.
 *
 * Returns ESP_OK on success.
 * Returns ESP_FAIL when the thermocouple is open or data is invalid.
 */
esp_err_t max6675_read_c(float *temperature_c);

bool max6675_is_initialized(void);

#ifdef __cplusplus
}
#endif
