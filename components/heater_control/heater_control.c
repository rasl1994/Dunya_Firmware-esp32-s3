#include "heater_control.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tca9554.h"

static const char *TAG = "heater_control";

static heater_control_config_t s_config;
static heater_control_state_t s_state;
static SemaphoreHandle_t s_mutex;

static uint8_t s_overcurrent_samples;
static uint8_t s_overpower_samples;
static uint8_t s_undervoltage_samples;
static uint8_t s_sensor_fault_samples;

static uint32_t heater_duty_max(void)
{
    const unsigned resolution_bits =
        (unsigned)s_config.duty_resolution;

    if (resolution_bits == 0 ||
        resolution_bits >= 31) {
        return 0;
    }

    return (1UL << resolution_bits) - 1UL;
}

static uint32_t heater_percent_to_duty(
    uint8_t percent
)
{
    return
        (heater_duty_max() *
         (uint32_t)percent +
         50U) /
        100U;
}

static bool heater_enable_level(bool enabled)
{
    return enabled
        ? s_config.enable_active_high
        : !s_config.enable_active_high;
}

static esp_err_t heater_apply_pwm_locked(
    uint8_t percent
)
{
    if (percent > s_config.maximum_percent) {
        percent = s_config.maximum_percent;
    }

    const uint32_t duty =
        heater_percent_to_duty(percent);

    ESP_RETURN_ON_ERROR(
        ledc_set_duty(
            s_config.speed_mode,
            s_config.channel,
            duty
        ),
        TAG,
        "Unable to set HEATER_PWM duty"
    );

    ESP_RETURN_ON_ERROR(
        ledc_update_duty(
            s_config.speed_mode,
            s_config.channel
        ),
        TAG,
        "Unable to update HEATER_PWM duty"
    );

    s_state.applied_percent = percent;
    return ESP_OK;
}

static esp_err_t heater_set_enable_locked(
    bool enabled
)
{
    const esp_err_t err =
        tca9554_set_level(
            s_config.enable_iox_pin,
            heater_enable_level(enabled)
        );

    if (err == ESP_OK) {
        s_state.driver_enabled = enabled;
    }

    return err;
}

static esp_err_t heater_disable_output_locked(void)
{
    esp_err_t first_error =
        heater_apply_pwm_locked(0);

    /*
     * Ensure the LEDC output has reached zero before disabling the
     * 12 V gate-driver supply through TCA9554 P3.
     */
    vTaskDelay(1);

    const esp_err_t enable_error =
        heater_set_enable_locked(false);

    if (first_error == ESP_OK) {
        first_error = enable_error;
    }

    s_state.driver_enabled = false;
    s_state.applied_percent = 0;

    return first_error;
}

static esp_err_t heater_enable_output_locked(void)
{
    if (s_state.fault_flags != HEATER_FAULT_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Safe startup:
     * 1. force PWM to zero;
     * 2. enable TLV61046/UCC27517 supply with TCA9554 P3;
     * 3. wait for startup;
     * 4. apply requested PWM.
     */
    ESP_RETURN_ON_ERROR(
        heater_apply_pwm_locked(0),
        TAG,
        "Unable to force HEATER_PWM low"
    );

    ESP_RETURN_ON_ERROR(
        heater_set_enable_locked(true),
        TAG,
        "Unable to enable HEATER_EN through TCA9554"
    );

    if (s_config.driver_startup_delay_ms > 0) {
        vTaskDelay(
            pdMS_TO_TICKS(
                s_config.driver_startup_delay_ms
            )
        );
    }

    return heater_apply_pwm_locked(
        s_state.requested_percent
    );
}

static void heater_reset_fault_counters_locked(void)
{
    s_overcurrent_samples = 0;
    s_overpower_samples = 0;
    s_undervoltage_samples = 0;
    s_sensor_fault_samples = 0;
}

static bool heater_increment_and_confirm(
    uint8_t *counter,
    bool bad
)
{
    if (!bad) {
        *counter = 0;
        return false;
    }

    if (*counter < UINT8_MAX) {
        ++(*counter);
    }

    return
        *counter >=
        s_config.fault_confirm_samples;
}

static esp_err_t heater_stop_with_fault_locked(
    uint32_t fault_flags
)
{
    s_state.fault_flags |= fault_flags;
    s_state.enabled = false;

    const esp_err_t err =
        heater_disable_output_locked();

    ESP_LOGE(
        TAG,
        "Heater stopped, fault flags=0x%08lX",
        (unsigned long)s_state.fault_flags
    );

    return err;
}

esp_err_t heater_control_init(
    const heater_control_config_t *config
)
{
    if (s_state.initialized) {
        return ESP_OK;
    }

    if (config == NULL ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->pwm_gpio) ||
        config->enable_iox_pin > 7 ||
        config->pwm_frequency_hz == 0 ||
        config->maximum_percent > 100 ||
        config->fault_confirm_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * The current project uses tca9554_set_level().
     * The I/O expander may already be initialized by display_power_init().
     */
    esp_err_t err = tca9554_init();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    /*
     * Put HEATER_EN into the inactive state before configuring PWM.
     */
    err = tca9554_set_level(
        s_config.enable_iox_pin,
        heater_enable_level(false)
    );

    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode = s_config.speed_mode,
        .duty_resolution =
            s_config.duty_resolution,
        .timer_num = s_config.timer,
        .freq_hz =
            (int)s_config.pwm_frequency_hz,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };

    err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num = s_config.pwm_gpio,
        .speed_mode = s_config.speed_mode,
        .channel = s_config.channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = s_config.timer,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode =
            LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {
            .output_invert = 0,
        },
    };

    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.initialized = true;

    ESP_LOGI(
        TAG,
        "Initialized: PWM GPIO=%d, HEATER_EN=TCA9554 P%u, frequency=%lu Hz",
        (int)s_config.pwm_gpio,
        (unsigned)s_config.enable_iox_pin,
        (unsigned long)s_config.pwm_frequency_hz
    );

    return ESP_OK;
}

esp_err_t heater_control_set_power_percent(
    uint8_t percent
)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (percent > 100) {
        percent = 100;
    }

    if (percent > s_config.maximum_percent) {
        percent = s_config.maximum_percent;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_state.requested_percent = percent;

    esp_err_t err = ESP_OK;

    if (s_state.enabled &&
        s_state.driver_enabled &&
        s_state.fault_flags ==
            HEATER_FAULT_NONE) {
        err = heater_apply_pwm_locked(percent);
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t heater_control_set_enabled(bool enabled)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_err_t err = ESP_OK;

    if (enabled) {
        if (s_state.fault_flags !=
            HEATER_FAULT_NONE) {
            err = ESP_ERR_INVALID_STATE;
        } else if (!s_state.enabled) {
            s_state.enabled = true;
            heater_reset_fault_counters_locked();

            err = heater_enable_output_locked();

            if (err != ESP_OK) {
                s_state.enabled = false;
                (void)heater_disable_output_locked();
            }
        }
    } else {
        s_state.enabled = false;
        err = heater_disable_output_locked();
        heater_reset_fault_counters_locked();
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t heater_control_emergency_stop(
    uint32_t fault_flags
)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (fault_flags == HEATER_FAULT_NONE) {
        fault_flags = HEATER_FAULT_MANUAL_STOP;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    const esp_err_t err =
        heater_stop_with_fault_locked(
            fault_flags
        );

    const heater_control_state_t state_copy =
        s_state;

    xSemaphoreGive(s_mutex);

    if (s_config.fault_cb != NULL) {
        s_config.fault_cb(
            state_copy.fault_flags,
            &state_copy,
            s_config.user_ctx
        );
    }

    return err;
}

esp_err_t heater_control_clear_faults(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_state.enabled ||
        s_state.driver_enabled) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_state.fault_flags = HEATER_FAULT_NONE;
    heater_reset_fault_counters_locked();

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Heater faults cleared");
    return ESP_OK;
}

esp_err_t heater_control_update_measurement(
    bool valid,
    float bus_voltage_v,
    float current_a,
    float power_w
)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_state.bus_voltage_v = bus_voltage_v;
    s_state.current_a = current_a;
    s_state.power_w = power_w;

    if (!s_state.enabled) {
        heater_reset_fault_counters_locked();
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    uint32_t faults = HEATER_FAULT_NONE;

    if (heater_increment_and_confirm(
            &s_sensor_fault_samples,
            !valid)) {
        faults |= HEATER_FAULT_SENSOR;
    }

    if (valid) {
        if (heater_increment_and_confirm(
                &s_overcurrent_samples,
                s_config.max_current_a > 0.0f &&
                fabsf(current_a) >
                    s_config.max_current_a)) {
            faults |= HEATER_FAULT_OVERCURRENT;
        }

        if (heater_increment_and_confirm(
                &s_overpower_samples,
                s_config.max_power_w > 0.0f &&
                power_w >
                    s_config.max_power_w)) {
            faults |= HEATER_FAULT_OVERPOWER;
        }

        if (heater_increment_and_confirm(
                &s_undervoltage_samples,
                s_config.min_bus_voltage_v > 0.0f &&
                bus_voltage_v <
                    s_config.min_bus_voltage_v)) {
            faults |= HEATER_FAULT_UNDERVOLTAGE;
        }
    }

    esp_err_t err = ESP_OK;
    heater_control_state_t state_copy = s_state;
    bool notify_fault = false;

    if (faults != HEATER_FAULT_NONE) {
        err = heater_stop_with_fault_locked(
            faults
        );

        state_copy = s_state;
        notify_fault = true;
    }

    xSemaphoreGive(s_mutex);

    if (notify_fault &&
        s_config.fault_cb != NULL) {
        s_config.fault_cb(
            state_copy.fault_flags,
            &state_copy,
            s_config.user_ctx
        );
    }

    return err;
}

heater_control_state_t heater_control_get_state(void)
{
    heater_control_state_t state = {0};

    if (s_mutex == NULL) {
        return state;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    state = s_state;
    xSemaphoreGive(s_mutex);

    return state;
}
