#include "tca9554.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define TCA9554_REG_INPUT         0x00
#define TCA9554_REG_OUTPUT        0x01
#define TCA9554_REG_POLARITY      0x02
#define TCA9554_REG_CONFIGURATION 0x03

static const char *TAG = "tca9554";

static i2c_master_bus_handle_t s_bus_handle;
static i2c_master_dev_handle_t s_device_handle;

static uint8_t s_output = 0x00;
static uint8_t s_configuration = 0xFF;
static bool s_initialized = false;

i2c_master_bus_handle_t tca9554_get_bus_handle(void)
{
    return s_initialized ? s_bus_handle : NULL;
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    if (!s_initialized || s_device_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t data[] = {
        reg,
        value,
    };

    return i2c_master_transmit(
        s_device_handle,
        data,
        sizeof(data),
        100
    );
}

esp_err_t tca9554_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_GPIO_I2C_SDA,
        .scl_io_num = BOARD_GPIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&bus_config, &s_bus_handle),
        TAG,
        "Failed to create I2C master bus"
    );

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TCA9554_ADDR,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };

    esp_err_t err = i2c_master_bus_add_device(
        s_bus_handle,
        &device_config,
        &s_device_handle
    );

    if (err != ESP_OK) {
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = NULL;
        return err;
    }

    /*
     * Сначала считаем устройство инициализированным,
     * чтобы write_reg() мог использовать handle.
     */
    s_initialized = true;

    /*
     * Безопасное начальное состояние всех выходов.
     */
    s_output = 0x00;

    err = write_reg(TCA9554_REG_OUTPUT, s_output);
    if (err != ESP_OK) {
        goto fail;
    }

    /*
     * 0 = output, 1 = input.
     * Сейчас все восемь выводов используются как выходы.
     */
    s_configuration = 0x00;

    err = write_reg(TCA9554_REG_CONFIGURATION, s_configuration);
    if (err != ESP_OK) {
        goto fail;
    }

    err = write_reg(TCA9554_REG_POLARITY, 0x00);
    if (err != ESP_OK) {
        goto fail;
    }

    ESP_LOGI(
        TAG,
        "Initialized at I2C address 0x%02X",
        BOARD_TCA9554_ADDR
    );

    return ESP_OK;

fail:
    s_initialized = false;

    if (s_device_handle != NULL) {
        i2c_master_bus_rm_device(s_device_handle);
        s_device_handle = NULL;
    }

    if (s_bus_handle != NULL) {
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = NULL;
    }

    return err;
}

esp_err_t tca9554_set_level(uint8_t pin, bool level)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pin > 7) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t new_output = s_output;

    if (level) {
        new_output |= (uint8_t)(1U << pin);
    } else {
        new_output &= (uint8_t)~(1U << pin);
    }

    esp_err_t err = write_reg(TCA9554_REG_OUTPUT, new_output);
    if (err != ESP_OK) {
        return err;
    }

    s_output = new_output;
    return ESP_OK;
}

esp_err_t tca9554_get_output(uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *value = s_output;
    return ESP_OK;
}