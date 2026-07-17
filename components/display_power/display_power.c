#include "display_power.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tca9554.h"

static const char *TAG = "display_power";
static bool s_initialized = false;

static esp_err_t backlight_pwm_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = BOARD_BL_PWM_SPEED_MODE,
        .duty_resolution = BOARD_BL_PWM_RESOLUTION,
        .timer_num = BOARD_BL_PWM_TIMER,
        .freq_hz = BOARD_BL_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };

    ESP_RETURN_ON_ERROR(
        ledc_timer_config(&timer_cfg),
        TAG,
        "LEDC timer configuration failed"
    );

    const ledc_channel_config_t channel_cfg = {
        .gpio_num = BOARD_GPIO_BL_PWM,
        .speed_mode = BOARD_BL_PWM_SPEED_MODE,
        .channel = BOARD_BL_PWM_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BOARD_BL_PWM_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {
            .output_invert = 0,
        },
    };

    return ledc_channel_config(&channel_cfg);
}

esp_err_t display_power_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(tca9554_init(), TAG, "TCA9554 initialization failed");
    ESP_RETURN_ON_ERROR(backlight_pwm_init(), TAG, "Backlight PWM initialization failed");

    /* Safe startup: PWM=0, backlight disabled, reset asserted, panel power off. */
    ESP_RETURN_ON_ERROR(
        display_backlight_set_percent(0),
        TAG,
        "Unable to set backlight duty to zero"
    );

    ESP_RETURN_ON_ERROR(
        tca9554_set_level(BOARD_IOX_BL_ENABLE, false),
        TAG,
        "Unable to disable backlight"
    );

    ESP_RETURN_ON_ERROR(
        tca9554_set_level(BOARD_IOX_LCD_RESET, false),
        TAG,
        "Unable to assert LCD reset"
    );

    ESP_RETURN_ON_ERROR(
        tca9554_set_level(BOARD_IOX_DISP_PWR, false),
        TAG,
        "Unable to disable display power"
    );

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized in safe OFF state");
    return ESP_OK;
}

esp_err_t display_power_enable(bool enable)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!enable) {
        /* Respect power-off order: backlight first, then reset, then panel power. */
        ESP_RETURN_ON_ERROR(display_backlight_set_percent(0), TAG, "PWM off failed");
        ESP_RETURN_ON_ERROR(display_backlight_enable(false), TAG, "BL_EN off failed");
        ESP_RETURN_ON_ERROR(
            tca9554_set_level(BOARD_IOX_LCD_RESET, false),
            TAG,
            "LCD reset assert failed"
        );
    }

    ESP_RETURN_ON_ERROR(
        tca9554_set_level(BOARD_IOX_DISP_PWR, enable),
        TAG,
        "Display power switching failed"
    );

    ESP_LOGI(TAG, "Display power %s", enable ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t display_reset(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(
        tca9554_set_level(BOARD_IOX_LCD_RESET, false),
        TAG,
        "LCD reset assert failed"
    );
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(
        tca9554_set_level(BOARD_IOX_LCD_RESET, true),
        TAG,
        "LCD reset release failed"
    );
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_LOGI(TAG, "LCD hardware reset complete");
    return ESP_OK;
}

esp_err_t display_backlight_enable(bool enable)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(
        tca9554_set_level(BOARD_IOX_BL_ENABLE, enable),
        TAG,
        "Backlight enable switching failed"
    );

    ESP_LOGI(TAG, "Backlight %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t display_backlight_set_percent(uint8_t percent)
{
    if (!s_initialized) {
        /* During init the PWM hardware already exists, so permit the initial zero duty. */
        if (percent != 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (percent > 100U) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t max_duty = (1UL << BOARD_BL_PWM_RESOLUTION_BITS) - 1UL;
    const uint32_t duty = (max_duty * percent) / 100U;

    ESP_RETURN_ON_ERROR(
        ledc_set_duty(BOARD_BL_PWM_SPEED_MODE, BOARD_BL_PWM_CHANNEL, duty),
        TAG,
        "Unable to set LEDC duty"
    );

    ESP_RETURN_ON_ERROR(
        ledc_update_duty(BOARD_BL_PWM_SPEED_MODE, BOARD_BL_PWM_CHANNEL),
        TAG,
        "Unable to update LEDC duty"
    );

    ESP_LOGI(TAG, "Backlight brightness: %u%%", (unsigned)percent);
    return ESP_OK;
}
