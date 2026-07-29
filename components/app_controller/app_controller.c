#include "app_controller.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "app_state.h"
#include "board_pins.h"
#include "esp_check.h"
#include "esp_log.h"
#include "heater_control.h"
#include "ina226.h"
#include "service_screen.h"
#include "touch_gt911.h"
#include "ui_home_dynamic.h"

static const char *TAG = "app_controller";

typedef struct {
    app_controller_config_t config;

    app_screen_t screen;

    bool initialized;
    bool home_initialized;
    bool service_initialized;
} app_controller_context_t;

static app_controller_context_t s_app;

typedef struct {
    bool valid;
    float voltage_v;
    float current_a;
    float power_w;
} service_measurement_message_t;

static QueueHandle_t s_service_measurement_queue;
static TaskHandle_t s_service_measurement_task;


/*
 * The service screen remains visually 0..100%.
 *
 * 0% is a real OFF state.
 * 5% is the first active step and corresponds to 20% real PWM.
 * Visual 5..100% is mapped linearly to real PWM 20..100%.
 */
#define HEATER_VISUAL_FIRST_STEP  5U
#define HEATER_PWM_FIRST_ACTIVE  20U
#define HEATER_PWM_MAX_PERCENT  100U

static uint8_t map_visual_percent_to_pwm(
    uint8_t visual_percent
)
{
    if (visual_percent > 100U) {
        visual_percent = 100U;
    }

    if (visual_percent == 0U) {
        return 0U;
    }

    if (visual_percent < HEATER_VISUAL_FIRST_STEP) {
        visual_percent = HEATER_VISUAL_FIRST_STEP;
    }

    const uint32_t visual_span =
        100U - HEATER_VISUAL_FIRST_STEP;

    const uint32_t pwm_span =
        HEATER_PWM_MAX_PERCENT -
        HEATER_PWM_FIRST_ACTIVE;

    const uint32_t scaled =
        (
            (uint32_t)(
                visual_percent -
                HEATER_VISUAL_FIRST_STEP
            ) *
            pwm_span +
            visual_span / 2U
        ) /
        visual_span;

    return (uint8_t)(
        HEATER_PWM_FIRST_ACTIVE +
        scaled
    );
}

/* --------------------------------------------------------------------------
 * Screen navigation
 * -------------------------------------------------------------------------- */

static esp_err_t app_show_home_internal(void)
{
    if (!s_app.home_initialized) {
        ui_home_config_t config =
            UI_HOME_CONFIG_DEFAULT();

        /*
         * The callback is assigned below after its declaration.
         */
        extern void app_controller_home_event_bridge(
            ui_home_event_t event,
            const ui_home_model_t *model,
            void *user_ctx
        );

        config.event_cb =
            app_controller_home_event_bridge;
        config.user_ctx = NULL;

        const ui_home_model_t model = {
            .power_on = false,
            .ai_enabled = false,
            .running = false,
            .paused = false,
            .target_temperature =
                s_app.config.initial_target_temperature,
            .current_temperature =
                s_app.config.initial_current_temperature,
            .battery_percent =
                s_app.config.initial_battery_percent,
        };

        ESP_RETURN_ON_ERROR(
            ui_home_init(&config, &model),
            TAG,
            "Home screen initialization failed"
        );

        s_app.home_initialized = true;
    } else {
        ESP_RETURN_ON_ERROR(
            ui_home_redraw_all(),
            TAG,
            "Home screen redraw failed"
        );
    }

    s_app.screen = APP_SCREEN_HOME;
    app_state_set(APP_STATE_HOME);

    return ESP_OK;
}

static esp_err_t app_show_service_internal(void)
{
    ESP_RETURN_ON_FALSE(
        s_app.service_initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Service screen is not initialized"
    );

    ESP_RETURN_ON_ERROR(
        service_screen_show(),
        TAG,
        "Unable to show service screen"
    );

    s_app.screen = APP_SCREEN_SERVICE;
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Heater
 * -------------------------------------------------------------------------- */

static void heater_fault_handler(
    uint32_t fault_flags,
    const heater_control_state_t *state,
    void *user_ctx
)
{
    (void)user_ctx;

    ESP_LOGE(
        TAG,
        "HEATER FAULT 0x%08lX: U=%.3f V I=%.3f A P=%.3f W",
        (unsigned long)fault_flags,
        state->bus_voltage_v,
        state->current_a,
        state->power_w
    );

    if (s_app.service_initialized) {
        service_screen_set_load_enabled(false);
    }
}

static esp_err_t heater_init(void)
{
    heater_control_config_t config =
        HEATER_CONTROL_CONFIG_DEFAULT();

    config.pwm_gpio =
        BOARD_GPIO_HEATER_PWM;

    config.enable_iox_pin =
        BOARD_IOX_HEATER_ENABLE;

    config.enable_active_high = true;

    /*
     * Backlight uses LEDC timer/channel 0.
     */
    config.timer = LEDC_TIMER_1;
    config.channel = LEDC_CHANNEL_1;

    config.pwm_frequency_hz =
        s_app.config.heater_pwm_frequency_hz;

    /*
     * The visual-to-PWM map requires the complete 0..100% hardware range.
     * Do not clamp it to an old 80% or 10% application setting.
     */
    config.maximum_percent =
        HEATER_PWM_MAX_PERCENT;

    config.driver_startup_delay_ms = 20;

    config.max_current_a =
        s_app.config.heater_max_current_a;

    config.max_power_w =
        s_app.config.heater_max_power_w;

    config.min_bus_voltage_v =
        s_app.config.heater_min_bus_voltage_v;

    config.fault_confirm_samples = 3;
    config.fault_cb = heater_fault_handler;
    config.user_ctx = NULL;

    return heater_control_init(&config);
}

static esp_err_t heater_apply(
    bool ui_enabled,
    uint8_t visual_percent
)
{
    const uint8_t pwm_percent =
        map_visual_percent_to_pwm(
            visual_percent
        );

    const bool output_enabled =
        ui_enabled &&
        visual_percent > 0U;

    ESP_RETURN_ON_ERROR(
        heater_control_set_power_percent(
            pwm_percent
        ),
        TAG,
        "Unable to set heater power"
    );

    esp_err_t err =
        heater_control_set_enabled(
            output_enabled
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Unable to change heater state: %s",
            esp_err_to_name(err)
        );

        if (s_app.service_initialized) {
            service_screen_set_load_enabled(false);
        }

        return err;
    }

    const heater_control_state_t state =
        heater_control_get_state();

    ESP_LOGI(
        TAG,
        "Heater: ui_enabled=%d output_enabled=%d "
        "visual=%u%% pwm_requested=%u%% "
        "pwm_applied=%u%% faults=0x%08lX",
        ui_enabled,
        state.enabled,
        (unsigned)visual_percent,
        (unsigned)state.requested_percent,
        (unsigned)state.applied_percent,
        (unsigned long)state.fault_flags
    );

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * UI callbacks
 * -------------------------------------------------------------------------- */

void app_controller_home_event_bridge(
    ui_home_event_t event,
    const ui_home_model_t *model,
    void *user_ctx
)
{
    (void)user_ctx;

    switch (event) {
        case UI_HOME_EVENT_POWER_TOGGLED:
            ESP_LOGI(
                TAG,
                "Home power: %s",
                model->power_on ? "ON" : "OFF"
            );
            break;

        case UI_HOME_EVENT_PLUS:
        case UI_HOME_EVENT_MINUS:
            ESP_LOGI(
                TAG,
                "Target temperature: %u",
                model->target_temperature
            );
            break;

        case UI_HOME_EVENT_AI_TOGGLED:
            ESP_LOGI(
                TAG,
                "AI mode: %s",
                model->ai_enabled ? "ON" : "OFF"
            );
            break;
    }
}

static void service_event_handler(
    service_screen_event_t event,
    const service_screen_model_t *model,
    void *user_ctx
)
{
    (void)user_ctx;

    switch (event) {
        case SERVICE_SCREEN_EVENT_BACK:
            /*
             * Safety policy: leaving the service screen always
             * switches the heater off.
             */
            (void)heater_control_set_enabled(false);
            (void)service_screen_hide();

            if (app_show_home_internal() != ESP_OK) {
                app_state_set(APP_STATE_ERROR);
            }
            break;

        case SERVICE_SCREEN_EVENT_LOAD_CHANGED:
        case SERVICE_SCREEN_EVENT_LOAD_ENABLED_CHANGED:
            (void)heater_apply(
                model->load_enabled,
                model->load_percent
            );
            break;
    }
}

/* --------------------------------------------------------------------------
 * Service-screen measurement dispatch
 * -------------------------------------------------------------------------- */

static void service_measurement_task(void *argument)
{
    (void)argument;

    service_measurement_message_t message;

    while (true) {
        if (xQueueReceive(
                s_service_measurement_queue,
                &message,
                portMAX_DELAY
            ) == pdTRUE) {
            service_screen_set_measurements(
                message.valid,
                message.voltage_v,
                message.current_a,
                message.power_w
            );
        }
    }
}

static esp_err_t service_measurement_dispatch_init(void)
{
    if (s_service_measurement_queue != NULL) {
        return ESP_OK;
    }

    s_service_measurement_queue =
        xQueueCreate(
            1,
            sizeof(service_measurement_message_t)
        );

    if (s_service_measurement_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t result =
        xTaskCreatePinnedToCore(
            service_measurement_task,
            "service_metrics",
            6144,
            NULL,
            3,
            &s_service_measurement_task,
            1
        );

    if (result != pdPASS) {
        vQueueDelete(s_service_measurement_queue);
        s_service_measurement_queue = NULL;
        s_service_measurement_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void service_measurement_publish(
    bool valid,
    float voltage_v,
    float current_a,
    float power_w
)
{
    if (s_service_measurement_queue == NULL) {
        return;
    }

    const service_measurement_message_t message = {
        .valid = valid,
        .voltage_v = voltage_v,
        .current_a = current_a,
        .power_w = power_w,
    };

    /* Queue length is one: always keep the newest sample. */
    (void)xQueueOverwrite(
        s_service_measurement_queue,
        &message
    );
}

/* --------------------------------------------------------------------------
 * INA226
 * -------------------------------------------------------------------------- */

static void ina226_measurement_handler(
    const ina226_measurement_t *measurement,
    void *user_ctx
)
{
    (void)user_ctx;

    /*
     * INA226 already supplies the final corrected current and power.
     * The application does not perform any electrical calculations.
     */
    const esp_err_t heater_err =
        heater_control_update_measurement(
            measurement->valid,
            measurement->bus_voltage_v,
            measurement->current_a,
            measurement->power_w
        );

    if (heater_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Heater measurement processing: %s",
            esp_err_to_name(heater_err)
        );
    }

    service_measurement_publish(
        measurement->valid,
        measurement->bus_voltage_v,
        measurement->current_a,
        measurement->power_w
    );

    ESP_LOGI(
        TAG,
        "INA226: U=%.3f V "
        "Iraw=%.4f A Ireg=%.4f A "
        "I=%.4f A P=%.3f W",
        measurement->bus_voltage_v,
        measurement->current_raw_a,
        measurement->current_register_a,
        measurement->current_a,
        measurement->power_w
    );
}

static esp_err_t ina226_init_and_start(void)
{
    ina226_config_t config =
        INA226_CONFIG_DEFAULT();

    config.i2c_address = 0x40;
    config.shunt_resistance_ohm = 0.01f;
    config.max_expected_current_a = 10.0f;

    /*
     * Start with one tunable coefficient instead of a point table.
     *
     * New factor after a reference measurement:
     *
     *     new_factor =
     *         old_factor *
     *         multimeter_current /
     *         displayed_current
     */
    config.current_correction_factor = 1.1f;
    config.current_zero_threshold_a = 0.03f;
    config.poll_period_ms = 100;

    config.measurement_cb =
        ina226_measurement_handler;
    config.user_ctx = NULL;

    ESP_RETURN_ON_ERROR(
        ina226_init(&config),
        TAG,
        "INA226 initialization failed"
    );

    return ina226_start();
}

/* --------------------------------------------------------------------------
 * Touch routing
 * -------------------------------------------------------------------------- */

static void touch_event_handler(
    const touch_gt911_event_t *event,
    void *user_ctx
)
{
    (void)user_ctx;

    switch (s_app.screen) {
        case APP_SCREEN_SERVICE:
            (void)service_screen_handle_touch(
                event->x,
                event->y,
                event->pressed
            );
            break;

        case APP_SCREEN_HOME:
            (void)ui_home_handle_touch(
                event->x,
                event->y,
                event->pressed
            );
            break;

        case APP_SCREEN_NONE:
        default:
            break;
    }
}

static esp_err_t touch_init_and_start(void)
{
    touch_gt911_config_t config =
        TOUCH_GT911_CONFIG_DEFAULT();

    config.x_max = 480;
    config.y_max = 480;

    config.swap_xy = false;
    config.mirror_x = false;
    config.mirror_y = false;

    config.poll_period_ms = 10;
    config.event_cb = touch_event_handler;
    config.user_ctx = NULL;

    ESP_RETURN_ON_ERROR(
        touch_gt911_init(&config),
        TAG,
        "GT911 initialization failed"
    );

    return touch_gt911_start();
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

esp_err_t app_controller_init(
    const app_controller_config_t *config
)
{
    ESP_RETURN_ON_FALSE(
        config != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Configuration is NULL"
    );

    if (s_app.initialized) {
        return ESP_OK;
    }

    memset(&s_app, 0, sizeof(s_app));
    s_app.config = *config;

    ESP_RETURN_ON_ERROR(
        heater_init(),
        TAG,
        "Heater initialization failed"
    );

    service_screen_config_t service_config =
        SERVICE_SCREEN_CONFIG_DEFAULT();

    service_config.event_cb =
        service_event_handler;

    const service_screen_model_t service_model = {
        .load_enabled = false,
        .load_percent = 0,
    };

    ESP_RETURN_ON_ERROR(
        service_screen_init(
            &service_config,
            &service_model
        ),
        TAG,
        "Service screen initialization failed"
    );

    s_app.service_initialized = true;

    ESP_RETURN_ON_ERROR(
        service_measurement_dispatch_init(),
        TAG,
        "Service measurement dispatcher initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        ina226_init_and_start(),
        TAG,
        "INA226 startup failed"
    );

    s_app.screen = APP_SCREEN_NONE;
    s_app.initialized = true;

    if (s_app.config.start_in_service_screen) {
        ESP_RETURN_ON_ERROR(
            app_show_service_internal(),
            TAG,
            "Unable to select startup screen"
        );
    }

    ESP_RETURN_ON_ERROR(
        touch_init_and_start(),
        TAG,
        "Touch startup failed"
    );

    return ESP_OK;
}

esp_err_t app_controller_show_home(void)
{
    ESP_RETURN_ON_FALSE(
        s_app.initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Controller is not initialized"
    );

    return app_show_home_internal();
}

esp_err_t app_controller_show_service(void)
{
    ESP_RETURN_ON_FALSE(
        s_app.initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Controller is not initialized"
    );

    return app_show_service_internal();
}

app_screen_t app_controller_get_screen(void)
{
    return s_app.screen;
}
