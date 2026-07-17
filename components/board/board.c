#include "board.h"

#include "esp_log.h"

static const char *TAG = "board";

esp_err_t board_init(void)
{
    /*
     * Safe first milestone: do not drive the display, backlight or heater
     * until all GPIO mappings and active levels are verified.
     */
    ESP_LOGI(TAG, "Board layer initialized in safe mode");
    return ESP_OK;
}
