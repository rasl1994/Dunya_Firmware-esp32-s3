#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize BL_PWM and put display control lines into a safe OFF state. */
esp_err_t display_power_init(void);

/** Enable or disable the display power rail through TCA9554. */
esp_err_t display_power_enable(bool enable);

/** Perform an active-low hardware reset pulse for the LCD controller. */
esp_err_t display_reset(void);

/** Enable or disable the backlight power stage through TCA9554. */
esp_err_t display_backlight_enable(bool enable);

/** Set PWM brightness from 0 to 100 percent. */
esp_err_t display_backlight_set_percent(uint8_t percent);

#ifdef __cplusplus
}
#endif
