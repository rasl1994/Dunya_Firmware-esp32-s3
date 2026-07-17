#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rgb_display_init(void);

esp_err_t rgb_display_fill(uint16_t color);

esp_err_t rgb_display_draw_bitmap(
    int x,
    int y,
    int width,
    int height,
    const uint16_t *pixels
);

/*
 * Returns the current back buffer.
 * Draw the complete frame or changed UI regions into this buffer.
 */
uint16_t *rgb_display_get_framebuffer(void);

/*
 * Requests the back buffer for display, waits for VSYNC, then prepares the
 * other framebuffer for the next partial UI update.
 */
esp_err_t rgb_display_present(void);

int rgb_display_get_width(void);
int rgb_display_get_height(void);

esp_lcd_panel_handle_t rgb_display_get_panel(void);

#ifdef __cplusplus
}
#endif
