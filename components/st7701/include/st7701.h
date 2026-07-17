#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the 3-wire SPI transport used by ST7701S. */
esp_err_t st7701_init(void);

/** Send the complete panel initialization sequence. */
esp_err_t st7701_panel_init(void);

/** Send one command byte over 3-wire SPI. */
esp_err_t st7701_write_command(uint8_t command);

/** Send one data byte over 3-wire SPI. */
esp_err_t st7701_write_data(uint8_t data);

#ifdef __cplusplus
}
#endif
