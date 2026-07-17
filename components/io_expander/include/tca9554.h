#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
i2c_master_bus_handle_t tca9554_get_bus_handle(void);
esp_err_t tca9554_init(void);
esp_err_t tca9554_set_level(uint8_t pin, bool level);
esp_err_t tca9554_get_output(uint8_t *value);

#ifdef __cplusplus
}
#endif