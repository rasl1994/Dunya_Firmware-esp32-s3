#include "touch_gt911.h"

#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tca9554.h"

#define GT911_ADDR_PRIMARY 0x5D
#define GT911_ADDR_ALTERNATIVE 0x14

#define GT911_REG_CONFIG 0x8047
#define GT911_REG_PRODUCT_ID 0x8140
#define GT911_REG_STATUS 0x814E
#define GT911_REG_FIRST_POINT 0x814F

#define GT911_STATUS_READY 0x80
#define GT911_STATUS_POINTS_MASK 0x0F

#define GT911_POINT_SIZE 8
#define GT911_MAX_POINTS 5

#define GT911_RELEASE_DEBOUNCE_SAMPLES 4

static const char *TAG = "touch_gt911";

static i2c_master_dev_handle_t s_device;
static touch_gt911_config_t s_config;
static TaskHandle_t s_task;
static bool s_initialized;
static bool s_last_pressed;
static uint16_t s_last_x;
static uint16_t s_last_y;
static uint16_t s_last_strength;

static esp_err_t gt911_write_reg16(uint16_t reg, const uint8_t *data, size_t len)
{
    if (s_device == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buffer[2 + 8];

    if (len > 8)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[0] = (uint8_t)(reg >> 8);
    buffer[1] = (uint8_t)(reg & 0xFF);

    if (data != NULL && len > 0)
    {
        memcpy(&buffer[2], data, len);
    }

    return i2c_master_transmit(
        s_device,
        buffer,
        len + 2,
        100);
}

static esp_err_t gt911_read_reg16(uint16_t reg, uint8_t *data, size_t len)
{
    if (s_device == NULL || data == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t address[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF),
    };

    return i2c_master_transmit_receive(
        s_device,
        address,
        sizeof(address),
        data,
        len,
        100);
}

static esp_err_t gt911_add_device(uint8_t address)
{
    i2c_master_bus_handle_t bus = tca9554_get_bus_handle();
    if (bus == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };

    return i2c_master_bus_add_device(
        bus,
        &device_config,
        &s_device);
}

static esp_err_t gt911_select_address_and_reset(uint8_t address)
{
    /*
     * GT911 address selection during reset:
     * INT=0 -> 0x5D
     * INT=1 -> 0x14
     */
    const int int_level =
        (address == GT911_ADDR_ALTERNATIVE) ? 1 : 0;

    const gpio_config_t int_output_config = {
        .pin_bit_mask = BIT64(BOARD_GPIO_TP_INT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(
        gpio_config(&int_output_config),
        TAG,
        "Unable to configure TP_INT as output");

    ESP_RETURN_ON_ERROR(
        gpio_set_level(BOARD_GPIO_TP_INT, int_level),
        TAG,
        "Unable to select GT911 address");

    ESP_RETURN_ON_ERROR(
        tca9554_set_level(BOARD_IOX_TP_RESET, false),
        TAG,
        "Unable to assert GT911 reset");

    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(
        tca9554_set_level(BOARD_IOX_TP_RESET, true),
        TAG,
        "Unable to release GT911 reset");

    vTaskDelay(pdMS_TO_TICKS(10));

    const gpio_config_t int_input_config = {
        .pin_bit_mask = BIT64(BOARD_GPIO_TP_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(
        gpio_config(&int_input_config),
        TAG,
        "Unable to configure TP_INT as input");

    vTaskDelay(pdMS_TO_TICKS(50));

    return ESP_OK;
}

static void gt911_transform_coordinates(uint16_t *x, uint16_t *y)
{
    if (s_config.swap_xy)
    {
        const uint16_t temporary = *x;
        *x = *y;
        *y = temporary;
    }

    if (s_config.mirror_x)
    {
        *x = (*x < s_config.x_max)
                 ? (uint16_t)(s_config.x_max - 1U - *x)
                 : 0;
    }

    if (s_config.mirror_y)
    {
        *y = (*y < s_config.y_max)
                 ? (uint16_t)(s_config.y_max - 1U - *y)
                 : 0;
    }

    if (*x >= s_config.x_max)
    {
        *x = s_config.x_max - 1U;
    }

    if (*y >= s_config.y_max)
    {
        *y = s_config.y_max - 1U;
    }
}

static esp_err_t gt911_probe_and_read_identity(uint8_t address)
{
    ESP_RETURN_ON_ERROR(
        gt911_select_address_and_reset(address),
        TAG,
        "GT911 reset failed");

    ESP_RETURN_ON_ERROR(
        gt911_add_device(address),
        TAG,
        "Unable to add GT911 I2C device at 0x%02X",
        address);

    uint8_t product_id[4] = {0};

    esp_err_t err = gt911_read_reg16(
        GT911_REG_PRODUCT_ID,
        product_id,
        sizeof(product_id));

    if (err != ESP_OK)
    {
        i2c_master_bus_rm_device(s_device);
        s_device = NULL;
        return err;
    }

    if ((product_id[0] == 0x00 && product_id[1] == 0x00) ||
        (product_id[0] == 0xFF && product_id[1] == 0xFF))
    {
        i2c_master_bus_rm_device(s_device);
        s_device = NULL;
        return ESP_FAIL;
    }

    uint8_t config_version = 0;
    ESP_RETURN_ON_ERROR(
        gt911_read_reg16(
            GT911_REG_CONFIG,
            &config_version,
            1),
        TAG,
        "Unable to read GT911 config version");

    ESP_LOGI(
        TAG,
        "GT911 detected at 0x%02X, product='%c%c%c%c', config=%u",
        address,
        product_id[0],
        product_id[1],
        product_id[2],
        product_id[3],
        (unsigned)config_version);

    return ESP_OK;
}

esp_err_t touch_gt911_init(const touch_gt911_config_t *config)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    if (config == NULL ||
        config->x_max == 0 ||
        config->y_max == 0 ||
        config->poll_period_ms == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * TCA9554 owns and creates the shared I2C master bus.
     * display_power_init() normally initializes it before this function.
     */
    ESP_RETURN_ON_ERROR(
        tca9554_init(),
        TAG,
        "Shared I2C/TCA9554 initialization failed");

    s_config = *config;

    esp_err_t err = gt911_probe_and_read_identity(GT911_ADDR_PRIMARY);
    if (err != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "GT911 not found at 0x%02X, trying 0x%02X",
            GT911_ADDR_PRIMARY,
            GT911_ADDR_ALTERNATIVE);

        err = gt911_probe_and_read_identity(GT911_ADDR_ALTERNATIVE);
    }

    ESP_RETURN_ON_ERROR(
        err,
        TAG,
        "GT911 was not found at either supported address");

    s_last_pressed = false;
    s_initialized = true;

    return ESP_OK;
}

esp_err_t touch_gt911_read(touch_gt911_event_t *event)
{
    if (!s_initialized || event == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(
        gt911_read_reg16(GT911_REG_STATUS, &status, 1),
        TAG,
        "Unable to read GT911 status");

    const uint8_t point_count =
        status & GT911_STATUS_POINTS_MASK;

    if ((status & GT911_STATUS_READY) == 0 ||
        point_count == 0 ||
        point_count > GT911_MAX_POINTS)
    {

        if ((status & GT911_STATUS_READY) != 0)
        {
            const uint8_t clear = 0;
            ESP_RETURN_ON_ERROR(
                gt911_write_reg16(
                    GT911_REG_STATUS,
                    &clear,
                    1),
                TAG,
                "Unable to clear GT911 status");
        }

        event->pressed = false;
        event->x = s_last_x;
        event->y = s_last_y;
        event->strength = s_last_strength;
        return ESP_OK;
    }

    uint8_t point[GT911_POINT_SIZE] = {0};

    ESP_RETURN_ON_ERROR(
        gt911_read_reg16(
            GT911_REG_FIRST_POINT,
            point,
            sizeof(point)),
        TAG,
        "Unable to read GT911 point");

    const uint8_t clear = 0;
    ESP_RETURN_ON_ERROR(
        gt911_write_reg16(
            GT911_REG_STATUS,
            &clear,
            1),
        TAG,
        "Unable to clear GT911 status");

    uint16_t x =
        (uint16_t)point[1] |
        ((uint16_t)point[2] << 8);

    uint16_t y =
        (uint16_t)point[3] |
        ((uint16_t)point[4] << 8);

    const uint16_t strength =
        (uint16_t)point[5] |
        ((uint16_t)point[6] << 8);

    gt911_transform_coordinates(&x, &y);

    s_last_x = x;
    s_last_y = y;
    s_last_strength = strength;

    event->x = x;
    event->y = y;
    event->strength = strength;
    event->pressed = true;

    return ESP_OK;
}

static void gt911_task(void *argument)
{
    (void)argument;

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period =
        pdMS_TO_TICKS(s_config.poll_period_ms);

    bool stable_pressed = false;
    uint8_t release_samples = 0;

    touch_gt911_event_t last_event = {
        .x = 0,
        .y = 0,
        .strength = 0,
        .pressed = false,
    };

    while (true)
    {
        touch_gt911_event_t sample = {0};

        const esp_err_t err = touch_gt911_read(&sample);
        if (err != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Touch read failed: %s",
                esp_err_to_name(err));

            vTaskDelay(pdMS_TO_TICKS(100));
            last_wake = xTaskGetTickCount();
            continue;
        }

        if (sample.pressed)
        {
            release_samples = 0;

            last_event = sample;
            last_event.pressed = true;

            /*
             * Send the first PRESS event and coordinate updates while held.
             * Repeated pressed=true events do not activate a button; the UI
             * activates it only after the debounced RELEASE event.
             */
            if (!stable_pressed)
            {
                stable_pressed = true;

                if (s_config.event_cb != NULL)
                {
                    s_config.event_cb(
                        &last_event,
                        s_config.user_ctx);
                }
            }
            else if (s_config.event_cb != NULL)
            {
                s_config.event_cb(
                    &last_event,
                    s_config.user_ctx);
            }
        }
        else if (stable_pressed)
        {
            if (release_samples <
                GT911_RELEASE_DEBOUNCE_SAMPLES)
            {
                ++release_samples;
            }

            /*
             * Report RELEASE only after several consecutive empty samples.
             * At 10 ms polling and four samples this is about 40 ms.
             */
            if (release_samples >=
                GT911_RELEASE_DEBOUNCE_SAMPLES)
            {
                stable_pressed = false;
                release_samples = 0;

                last_event.pressed = false;

                if (s_config.event_cb != NULL)
                {
                    s_config.event_cb(
                        &last_event,
                        s_config.user_ctx);
                }
            }
        }
        else
        {
            release_samples = 0;
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t touch_gt911_start(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task != NULL)
    {
        return ESP_OK;
    }

    const BaseType_t result = xTaskCreatePinnedToCore(
        gt911_task,
        "gt911",
        4096,
        NULL,
        5,
        &s_task,
        1);

    if (result != pdPASS)
    {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Polling started, period=%lu ms",
        (unsigned long)s_config.poll_period_ms);

    return ESP_OK;
}

esp_err_t touch_gt911_stop(void)
{
    if (s_task == NULL)
    {
        return ESP_OK;
    }

    vTaskDelete(s_task);
    s_task = NULL;
    s_last_pressed = false;

    return ESP_OK;
}

bool touch_gt911_is_initialized(void)
{
    return s_initialized;
}
