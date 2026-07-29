#include "MAX6675.h"

#include <math.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tca9554.h"

static const char *TAG = "max6675";

/*
 * Hardware mapping supplied for this board.
 */
#define MAX6675_MISO_GPIO  GPIO_NUM_19
#define MAX6675_SCK_GPIO   GPIO_NUM_20
#define MAX6675_CS_IOX_PIN 6U

#define MAX6675_READ_BITS 16U
#define MAX6675_OPEN_BIT  0x0004U

static bool s_initialized;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_mutex;

static max6675_temperature_cb_t s_callback;
static void *s_callback_user_ctx;

static esp_err_t max6675_cs_set(bool active)
{
    /*
     * MAX6675 CS is active low.
     */
    return tca9554_set_level(
        MAX6675_CS_IOX_PIN,
        !active
    );
}

esp_err_t max6675_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(
        tca9554_init(),
        TAG,
        "TCA9554 initialization failed"
    );

    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(
        s_mutex != NULL,
        ESP_ERR_NO_MEM,
        TAG,
        "Unable to create MAX6675 mutex"
    );

    const gpio_config_t sck_config = {
        .pin_bit_mask = 1ULL << MAX6675_SCK_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&sck_config);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    const gpio_config_t miso_config = {
        .pin_bit_mask = 1ULL << MAX6675_MISO_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&miso_config);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    ESP_RETURN_ON_ERROR(
        gpio_set_level(MAX6675_SCK_GPIO, 0),
        TAG,
        "Unable to set SCK low"
    );

    err = max6675_cs_set(false);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    s_initialized = true;

    ESP_LOGI(
        TAG,
        "Initialized: MISO=GPIO%d SCK=GPIO%d CS=TCA9554 P%u",
        (int)MAX6675_MISO_GPIO,
        (int)MAX6675_SCK_GPIO,
        (unsigned)MAX6675_CS_IOX_PIN
    );

    return ESP_OK;
}

bool max6675_is_initialized(void)
{
    return s_initialized;
}

esp_err_t max6675_set_callback(
    max6675_temperature_cb_t callback,
    void *user_ctx
)
{
    s_callback = callback;
    s_callback_user_ctx = user_ctx;
    return ESP_OK;
}

static esp_err_t max6675_read_raw(uint16_t *raw_value)
{
    ESP_RETURN_ON_FALSE(
        raw_value != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Raw output pointer is NULL"
    );

    ESP_RETURN_ON_FALSE(
        s_initialized && s_mutex != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "MAX6675 is not initialized"
    );

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_err_t err =
        gpio_set_level(MAX6675_SCK_GPIO, 0);

    if (err == ESP_OK) {
        err = max6675_cs_set(true);
    }

    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    /*
     * Give the converter time to place D15 on SO.
     */
    esp_rom_delay_us(10);

    uint16_t value = 0U;

    for (uint32_t bit_index = 0U;
         bit_index < MAX6675_READ_BITS;
         ++bit_index) {
        gpio_set_level(MAX6675_SCK_GPIO, 1);
        esp_rom_delay_us(2);

        value = (uint16_t)(
            (value << 1U) |
            (uint16_t)(gpio_get_level(MAX6675_MISO_GPIO) & 1)
        );

        gpio_set_level(MAX6675_SCK_GPIO, 0);
        esp_rom_delay_us(2);
    }

    const esp_err_t cs_err =
        max6675_cs_set(false);

    xSemaphoreGive(s_mutex);

    if (cs_err != ESP_OK) {
        return cs_err;
    }

    *raw_value = value;
    return ESP_OK;
}

esp_err_t max6675_read_c(float *temperature_c)
{
    ESP_RETURN_ON_FALSE(
        temperature_c != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Temperature output pointer is NULL"
    );

    uint16_t raw = 0U;

    ESP_RETURN_ON_ERROR(
        max6675_read_raw(&raw),
        TAG,
        "MAX6675 raw read failed"
    );

    /*
     * D2 = 1 means an open thermocouple.
     * 0xFFFF normally means SO is floating or stuck high.
     */
    if ((raw & MAX6675_OPEN_BIT) != 0U ||
        raw == 0xFFFFU) {
        return ESP_FAIL;
    }

    const uint16_t temperature_raw =
        (uint16_t)((raw >> 3U) & 0x0FFFU);

    const float value_c =
        (float)temperature_raw * 0.25f;

    if (!isfinite(value_c)) {
        return ESP_FAIL;
    }

    *temperature_c = value_c;
    return ESP_OK;
}

static void max6675_task(void *argument)
{
    const uint32_t period_ms =
        (uint32_t)(uintptr_t)argument;

    TickType_t last_wake_time =
        xTaskGetTickCount();

    while (true) {
        float temperature_c = 0.0f;

        const bool valid =
            max6675_read_c(&temperature_c) ==
            ESP_OK;

        if (valid) {
            ESP_LOGI(
                TAG,
                "Temperature: %.2f C",
                (double)temperature_c
            );
        } else {
            ESP_LOGW(
                TAG,
                "Thermocouple open or invalid data"
            );
        }

        if (s_callback != NULL) {
            s_callback(
                valid,
                valid ? temperature_c : 0.0f,
                s_callback_user_ctx
            );
        }

        vTaskDelayUntil(
            &last_wake_time,
            pdMS_TO_TICKS(period_ms)
        );
    }
}

esp_err_t max6675_start_task(uint32_t period_ms)
{
    ESP_RETURN_ON_FALSE(
        s_initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "MAX6675 is not initialized"
    );

    ESP_RETURN_ON_FALSE(
        period_ms > 0U,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Polling period must be greater than zero"
    );

    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t result =
        xTaskCreatePinnedToCore(
            max6675_task,
            "max6675",
            3072,
            (void *)(uintptr_t)period_ms,
            4,
            &s_task,
            1
        );

    return result == pdPASS
        ? ESP_OK
        : ESP_ERR_NO_MEM;
}
