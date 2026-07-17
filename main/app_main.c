#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_state.h"
#include "board.h"
#include "display_power.h"
#include "intro_animation.h"
#include "rgb_display.h"
#include "st7701.h"
#include "touch_gt911.h"
#include "ui_home_dynamic.h"

static const char *TAG = "app";

static esp_err_t app_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void touch_event_handler(
    const touch_gt911_event_t *event,
    void *user_ctx)
{
    (void)user_ctx;

    /*
     * ui_home_handle_touch() is expected to receive both press and release.
     * Rename this call if your ui_home_dynamic header uses another name.
     */
    ui_home_handle_touch(
        event->x,
        event->y,
        event->pressed);
}

static void home_event_handler(ui_home_event_t event,
                               const ui_home_model_t *model,
                               void *user_ctx)
{
    (void)user_ctx;
    switch (event)
    {
    case UI_HOME_EVENT_POWER_TOGGLED:
        ESP_LOGI(TAG, "Power: %s", model->power_on ? "ON" : "OFF");
        /* Send a command to the device controller here. */
        break;
    case UI_HOME_EVENT_PLUS:
    case UI_HOME_EVENT_MINUS:
        ESP_LOGI(TAG, "Target temperature: %u", model->target_temperature);
        break;
    case UI_HOME_EVENT_AI_TOGGLED:
        ESP_LOGI(TAG, "AI mode: %s", model->ai_enabled ? "ON" : "OFF");
        break;
    }
}

static esp_err_t app_touch_init(void)
{
    touch_gt911_config_t config =
        TOUCH_GT911_CONFIG_DEFAULT();

    config.x_max = 480;
    config.y_max = 480;

    /*
     * Start with all transforms disabled.
     * Adjust these after checking the logged/raw coordinates.
     */
    config.swap_xy = false;
    config.mirror_x = false;
    config.mirror_y = false;

    config.poll_period_ms = 10;
    config.event_cb = touch_event_handler;
    config.user_ctx = NULL;

    ESP_RETURN_ON_ERROR(
        touch_gt911_init(&config),
        TAG,
        "GT911 initialization failed");

    return touch_gt911_start();
}

static esp_err_t show_home_screen(void)
{
    ui_home_config_t cfg = UI_HOME_CONFIG_DEFAULT();
    cfg.event_cb = home_event_handler;

    ui_home_model_t model = {
        .power_on = false,
        .ai_enabled = false,
        .running = false,
        .paused = false,
        .target_temperature = 99,
        .current_temperature = 25,
        .battery_percent = 0,
    };

    return ui_home_init(&cfg, &model);
}

static void intro_task(void *argument)
{
    (void)argument;

    intro_animation_config_t config =
        INTRO_ANIMATION_CONFIG_DEFAULT();

    config.frame_count = 50;
    config.frame_period_ms = 33; /* 30 FPS */
    config.loop = false;

    const esp_err_t err = intro_animation_play(&config);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Intro animation failed: %s", esp_err_to_name(err));
        app_state_set(APP_STATE_ERROR);
    }
    else
    {
        ESP_LOGI(TAG, "Intro animation completed");

        app_state_set(APP_STATE_HOME);

        const esp_err_t ui_err = show_home_screen();
        if (ui_err != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Home screen failed: %s",
                esp_err_to_name(ui_err));

            app_state_set(APP_STATE_ERROR);
        }
    }

    vTaskDelete(NULL);
}

static void battery_test_task(void *argument)
{
    (void)argument;

    while (app_state_get() != APP_STATE_HOME)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /*
     * APP_STATE_HOME устанавливается немного раньше завершения
     * ui_home_init(), поэтому даём экрану закончить инициализацию.
     */
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Battery indicator test started");

    while (true)
    {
        for (int level = 0; level <= 100; level += 5)
        {
            ESP_LOGI(TAG, "Battery level: %d%%", level);

            ui_home_set_battery_percent((uint8_t)level);

            vTaskDelay(pdMS_TO_TICKS(500));
        }

        for (int level = 100; level >= 0; level -= 5)
        {
            ESP_LOGI(TAG, "Battery level: %d%%", level);

            ui_home_set_battery_percent((uint8_t)level);

            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(app_nvs_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(display_power_init());

    ESP_ERROR_CHECK(display_power_enable(true));
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_ERROR_CHECK(display_reset());
    ESP_ERROR_CHECK(st7701_init());
    ESP_ERROR_CHECK(st7701_panel_init());

    ESP_ERROR_CHECK(rgb_display_init());
    ESP_ERROR_CHECK(rgb_display_fill(0x0000));

    ESP_ERROR_CHECK(app_touch_init());

    /* SPIFFS must be mounted before the animation task opens frame files. */
    ESP_ERROR_CHECK(intro_animation_mount_spiffs());

    ESP_ERROR_CHECK(display_backlight_set_percent(20));
    ESP_ERROR_CHECK(display_backlight_enable(true));

    app_state_set(APP_STATE_STARTUP_ANIMATION);

    const BaseType_t result = xTaskCreatePinnedToCore(
        intro_task,
        "intro_animation",
        8192,
        NULL,
        4,
        NULL,
        1);

    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Animation task creation failed");
        app_state_set(APP_STATE_ERROR);
    }

    const BaseType_t battery_test_result = xTaskCreatePinnedToCore(
        battery_test_task,
        "battery_test",
        4096,
        NULL,
        3,
        NULL,
        1);

    if (battery_test_result != pdPASS)
    {
        ESP_LOGE(TAG, "Battery test task creation failed");
    }
}
