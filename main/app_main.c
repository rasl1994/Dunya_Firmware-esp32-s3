#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_controller.h"
#include "app_state.h"
#include "board.h"
#include "display_power.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "intro_animation.h"
#include "nvs_flash.h"
#include "rgb_display.h"
#include "st7701.h"

static const char *TAG = "app";

/*
 * 1: immediately open the service/heater screen.
 * 0: play the intro and open the home screen.
 */
#define HEATER_TEST_MODE 1

static esp_err_t app_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    return err;
}

static void intro_task(void *argument)
{
    (void)argument;

    intro_animation_config_t config =
        INTRO_ANIMATION_CONFIG_DEFAULT();

    config.frame_count = 10;
    config.frame_period_ms = 33;
    config.loop = false;

    const esp_err_t err =
        intro_animation_play(&config);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Intro animation failed: %s",
            esp_err_to_name(err)
        );

        app_state_set(APP_STATE_ERROR);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Intro animation completed");

    if (app_controller_show_home() != ESP_OK) {
        ESP_LOGE(TAG, "Unable to show home screen");
        app_state_set(APP_STATE_ERROR);
    }

    vTaskDelete(NULL);
}

static esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(
        display_power_enable(true),
        TAG,
        "Display power enable failed"
    );

    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(
        display_reset(),
        TAG,
        "Display reset failed"
    );

    ESP_RETURN_ON_ERROR(
        st7701_init(),
        TAG,
        "ST7701 bus initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        st7701_panel_init(),
        TAG,
        "ST7701 panel initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        rgb_display_init(),
        TAG,
        "RGB display initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        rgb_display_fill(0x0000),
        TAG,
        "Display clear failed"
    );

    ESP_RETURN_ON_ERROR(
        display_backlight_set_percent(20),
        TAG,
        "Backlight level failed"
    );

    return display_backlight_enable(true);
}

void app_main(void)
{
    ESP_ERROR_CHECK(app_nvs_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(display_power_init());
    ESP_ERROR_CHECK(display_init());

    app_controller_config_t controller_config =
        APP_CONTROLLER_CONFIG_DEFAULT();

    controller_config.start_in_service_screen =
        HEATER_TEST_MODE != 0;

    /*
     * Keep the safe testing limit until the heater and protection
     * thresholds are fully verified.
     */
    controller_config.heater_maximum_percent = 95;
    controller_config.heater_pwm_frequency_hz = 10000;
    ESP_ERROR_CHECK(
        app_controller_init(&controller_config)
    );

#if HEATER_TEST_MODE
    app_state_set(APP_STATE_HOME);

    ESP_LOGI(
        TAG,
        "Heater test mode ready. Set 5%% and press ON."
    );
#else
    ESP_ERROR_CHECK(
        intro_animation_mount_spiffs()
    );

    app_state_set(APP_STATE_STARTUP_ANIMATION);

    const BaseType_t result =
        xTaskCreatePinnedToCore(
            intro_task,
            "intro_animation",
            8192,
            NULL,
            4,
            NULL,
            1
        );

    if (result != pdPASS) {
        ESP_LOGE(
            TAG,
            "Animation task creation failed"
        );

        app_state_set(APP_STATE_ERROR);
    }
#endif
}
