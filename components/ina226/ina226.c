#include "ina226.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tca9554.h"

#define INA226_REG_CONFIG         0x00
#define INA226_REG_SHUNT_VOLTAGE  0x01
#define INA226_REG_BUS_VOLTAGE    0x02
#define INA226_REG_POWER          0x03
#define INA226_REG_CURRENT        0x04
#define INA226_REG_CALIBRATION    0x05
#define INA226_REG_MANUFACTURER   0xFE
#define INA226_REG_DIE_ID         0xFF

#define INA226_CONFIG_RESET         0x8000
#define INA226_CONFIG_RESERVED_D14  0x4000
#define INA226_CONFIG_AVG_16        0x0400
#define INA226_CONFIG_VBUS_1100     0x0100
#define INA226_CONFIG_VSHUNT_1100   0x0020
#define INA226_MODE_CONTINUOUS      0x0007
#define INA226_MODE_BUS_TRIGGERED   0x0002

#define INA226_CONFIG_NORMAL \
    (INA226_CONFIG_RESERVED_D14 | \
     INA226_CONFIG_AVG_16 | \
     INA226_CONFIG_VBUS_1100 | \
     INA226_CONFIG_VSHUNT_1100 | \
     INA226_MODE_CONTINUOUS)

#define INA226_CONFIG_FAST_VBUS \
    (INA226_CONFIG_RESERVED_D14 | \
     INA226_MODE_BUS_TRIGGERED)

#define INA226_MANUFACTURER_ID  0x5449
#define INA226_DIE_ID_MASK      0xFFF0
#define INA226_DIE_ID           0x2260

#define INA226_BUS_LSB_V        0.00125f
#define INA226_SHUNT_LSB_MV     0.0025f

static const char *TAG = "ina226";

static ina226_config_t s_config;
static i2c_master_dev_handle_t s_device;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_io_mutex;
static SemaphoreHandle_t s_data_mutex;
static ina226_measurement_t s_latest;
static bool s_initialized;
static bool s_running;
static float s_current_lsb_a;
static float s_power_lsb_w;
static uint16_t s_calibration;

static esp_err_t write_register_unlocked(
    uint8_t reg,
    uint16_t value
)
{
    if (s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t data[3] = {
        reg,
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF),
    };

    return i2c_master_transmit(
        s_device,
        data,
        sizeof(data),
        100
    );
}

static esp_err_t read_register_unlocked(
    uint8_t reg,
    uint16_t *value
)
{
    if (s_device == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[2] = {0};

    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(
            s_device,
            &reg,
            1,
            data,
            sizeof(data),
            100
        ),
        TAG,
        "Register 0x%02X read failed",
        reg
    );

    *value =
        ((uint16_t)data[0] << 8) |
        (uint16_t)data[1];

    return ESP_OK;
}

static esp_err_t configure_device(void)
{
    uint16_t manufacturer = 0;
    uint16_t die_id = 0;

    ESP_RETURN_ON_ERROR(
        read_register_unlocked(
            INA226_REG_MANUFACTURER,
            &manufacturer
        ),
        TAG,
        "Manufacturer read failed"
    );

    ESP_RETURN_ON_ERROR(
        read_register_unlocked(
            INA226_REG_DIE_ID,
            &die_id
        ),
        TAG,
        "Die ID read failed"
    );

    ESP_RETURN_ON_FALSE(
        manufacturer == INA226_MANUFACTURER_ID,
        ESP_ERR_NOT_FOUND,
        TAG,
        "Unexpected manufacturer ID 0x%04X",
        manufacturer
    );

    ESP_RETURN_ON_FALSE(
        (die_id & INA226_DIE_ID_MASK) ==
            INA226_DIE_ID,
        ESP_ERR_NOT_FOUND,
        TAG,
        "Unexpected die ID 0x%04X",
        die_id
    );

    ESP_LOGI(
        TAG,
        "INA226 found at 0x%02X, manufacturer=0x%04X die=0x%04X",
        s_config.i2c_address,
        manufacturer,
        die_id
    );

    ESP_RETURN_ON_ERROR(
        write_register_unlocked(
            INA226_REG_CONFIG,
            INA226_CONFIG_RESET
        ),
        TAG,
        "Reset failed"
    );

    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(
        write_register_unlocked(
            INA226_REG_CONFIG,
            INA226_CONFIG_NORMAL
        ),
        TAG,
        "Configuration failed"
    );

    ESP_RETURN_ON_ERROR(
        write_register_unlocked(
            INA226_REG_CALIBRATION,
            s_calibration
        ),
        TAG,
        "Calibration failed"
    );

    uint16_t config_readback = 0;
    uint16_t calibration_readback = 0;

    ESP_RETURN_ON_ERROR(
        read_register_unlocked(
            INA226_REG_CONFIG,
            &config_readback
        ),
        TAG,
        "CONFIG readback failed"
    );

    ESP_RETURN_ON_ERROR(
        read_register_unlocked(
            INA226_REG_CALIBRATION,
            &calibration_readback
        ),
        TAG,
        "CAL readback failed"
    );

    ESP_LOGI(
        TAG,
        "Register readback: CONFIG=0x%04X CAL=0x%04X",
        config_readback,
        calibration_readback
    );

    return ESP_OK;
}

esp_err_t ina226_init(
    const ina226_config_t *config
)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (config == NULL ||
        config->i2c_address > 0x7F ||
        config->shunt_resistance_ohm <= 0.0f ||
        config->max_expected_current_a <= 0.0f ||
        !isfinite(config->current_correction_factor) ||
        config->current_correction_factor <= 0.0f ||
        !isfinite(config->current_zero_threshold_a) ||
        config->current_zero_threshold_a < 0.0f ||
        config->poll_period_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(
        tca9554_init(),
        TAG,
        "Shared I2C initialization failed"
    );

    i2c_master_bus_handle_t bus =
        tca9554_get_bus_handle();

    ESP_RETURN_ON_FALSE(
        bus != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Shared I2C bus handle is NULL"
    );

    s_config = *config;

    s_current_lsb_a =
        s_config.max_expected_current_a /
        32768.0f;

    s_power_lsb_w =
        25.0f * s_current_lsb_a;

    const float calibration =
        0.00512f /
        (
            s_current_lsb_a *
            s_config.shunt_resistance_ohm
        );

    ESP_RETURN_ON_FALSE(
        calibration >= 1.0f &&
        calibration <= 65535.0f,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Calibration out of range: %.3f",
        calibration
    );

    s_calibration =
        (uint16_t)lroundf(calibration);

    const i2c_device_config_t device_config = {
        .dev_addr_length =
            I2C_ADDR_BIT_LEN_7,
        .device_address =
            s_config.i2c_address,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };

    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(
            bus,
            &device_config,
            &s_device
        ),
        TAG,
        "Unable to add INA226 to I2C bus"
    );

    s_io_mutex = xSemaphoreCreateMutex();
    s_data_mutex = xSemaphoreCreateMutex();

    if (s_io_mutex == NULL ||
        s_data_mutex == NULL) {
        if (s_io_mutex != NULL) {
            vSemaphoreDelete(s_io_mutex);
            s_io_mutex = NULL;
        }

        if (s_data_mutex != NULL) {
            vSemaphoreDelete(s_data_mutex);
            s_data_mutex = NULL;
        }

        i2c_master_bus_rm_device(s_device);
        s_device = NULL;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_io_mutex, portMAX_DELAY);
    const esp_err_t configure_err =
        configure_device();
    xSemaphoreGive(s_io_mutex);

    if (configure_err != ESP_OK) {
        i2c_master_bus_rm_device(s_device);
        s_device = NULL;
        vSemaphoreDelete(s_io_mutex);
        vSemaphoreDelete(s_data_mutex);
        s_io_mutex = NULL;
        s_data_mutex = NULL;
        return configure_err;
    }

    memset(&s_latest, 0, sizeof(s_latest));
    s_initialized = true;

    ESP_LOGI(
        TAG,
        "Initialized: shunt=%.6f ohm max=%.3f A "
        "factor=%.5f zero=%.3f A LSB=%.9f A cal=%u",
        s_config.shunt_resistance_ohm,
        s_config.max_expected_current_a,
        s_config.current_correction_factor,
        s_config.current_zero_threshold_a,
        s_current_lsb_a,
        (unsigned)s_calibration
    );

    return ESP_OK;
}

esp_err_t ina226_read(
    ina226_measurement_t *measurement
)
{
    if (!s_initialized ||
        measurement == NULL ||
        s_io_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t raw_shunt = 0;
    uint16_t raw_bus = 0;
    uint16_t raw_current = 0;
    uint16_t raw_power = 0;

    xSemaphoreTake(s_io_mutex, portMAX_DELAY);

    esp_err_t err =
        read_register_unlocked(
            INA226_REG_SHUNT_VOLTAGE,
            &raw_shunt
        );

    if (err == ESP_OK) {
        err = read_register_unlocked(
            INA226_REG_BUS_VOLTAGE,
            &raw_bus
        );
    }

    if (err == ESP_OK) {
        err = read_register_unlocked(
            INA226_REG_CURRENT,
            &raw_current
        );
    }

    if (err == ESP_OK) {
        err = read_register_unlocked(
            INA226_REG_POWER,
            &raw_power
        );
    }

    xSemaphoreGive(s_io_mutex);

    if (err != ESP_OK) {
        return err;
    }

    memset(measurement, 0, sizeof(*measurement));

    measurement->shunt_voltage_mv =
        (float)(int16_t)raw_shunt *
        INA226_SHUNT_LSB_MV;

    measurement->bus_voltage_v =
        (float)raw_bus *
        INA226_BUS_LSB_V;

    /*
     * All application-facing current and power calculations live here.
     *
     * Use VSHUNT directly as the primary current source so the result is
     * independent of the hardware CURRENT-register calibration rounding.
     */
    measurement->current_raw_a =
        (
            measurement->shunt_voltage_mv /
            1000.0f
        ) /
        s_config.shunt_resistance_ohm;

    measurement->current_register_a =
        (float)(int16_t)raw_current *
        s_current_lsb_a;

    if (fabsf(measurement->current_raw_a) <
        s_config.current_zero_threshold_a) {
        measurement->current_raw_a = 0.0f;
    }

    measurement->current_a =
        measurement->current_raw_a *
        s_config.current_correction_factor;

    /*
     * Power shown to the application is calculated from the bus voltage
     * measured under load and the final corrected current.
     */
    measurement->power_w =
        measurement->bus_voltage_v *
        measurement->current_a;

    measurement->power_register_w =
        (float)raw_power *
        s_power_lsb_w;

    measurement->valid =
        isfinite(measurement->bus_voltage_v) &&
        isfinite(measurement->current_a) &&
        isfinite(measurement->power_w);

    return ESP_OK;
}

esp_err_t ina226_read_bus_voltage_triggered(
    float *bus_voltage_v
)
{
    if (!s_initialized ||
        bus_voltage_v == NULL ||
        s_io_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t raw_bus = 0;
    esp_err_t err = ESP_OK;
    esp_err_t restore_err = ESP_OK;

    xSemaphoreTake(s_io_mutex, portMAX_DELAY);

    err = write_register_unlocked(
        INA226_REG_CONFIG,
        INA226_CONFIG_FAST_VBUS
    );

    if (err == ESP_OK) {
        /* 140 us bus conversion plus margin. */
        esp_rom_delay_us(300);

        err = read_register_unlocked(
            INA226_REG_BUS_VOLTAGE,
            &raw_bus
        );
    }

    restore_err = write_register_unlocked(
        INA226_REG_CONFIG,
        INA226_CONFIG_NORMAL
    );

    xSemaphoreGive(s_io_mutex);

    if (err != ESP_OK) {
        return err;
    }

    if (restore_err != ESP_OK) {
        return restore_err;
    }

    *bus_voltage_v =
        (float)raw_bus *
        INA226_BUS_LSB_V;

    return ESP_OK;
}

static void poll_task(void *argument)
{
    (void)argument;

    TickType_t last_wake =
        xTaskGetTickCount();

    const TickType_t period =
        pdMS_TO_TICKS(
            s_config.poll_period_ms
        );

    uint32_t sample_number = 0;

    ESP_LOGI(
        TAG,
        "Polling started, period=%lu ms",
        (unsigned long)s_config.poll_period_ms
    );

    while (true) {
        ina226_measurement_t measurement = {0};

        const esp_err_t err =
            ina226_read(&measurement);

        if (err == ESP_OK) {
            measurement.sample_number =
                ++sample_number;

            xSemaphoreTake(
                s_data_mutex,
                portMAX_DELAY
            );

            s_latest = measurement;

            xSemaphoreGive(s_data_mutex);

            if (s_config.measurement_cb != NULL) {
                s_config.measurement_cb(
                    &measurement,
                    s_config.user_ctx
                );
            }

            ESP_LOGI(
                TAG,
                "U=%.3f V Vsh=%.3f mV "
                "Iraw=%.4f A Ireg=%.4f A "
                "K=%.4f I=%.4f A "
                "P=%.3f W Preg=%.3f W",
                measurement.bus_voltage_v,
                measurement.shunt_voltage_mv,
                measurement.current_raw_a,
                measurement.current_register_a,
                s_config.current_correction_factor,
                measurement.current_a,
                measurement.power_w,
                measurement.power_register_w
            );
        } else {
            ESP_LOGW(
                TAG,
                "Measurement failed: %s",
                esp_err_to_name(err)
            );
        }

        vTaskDelayUntil(
            &last_wake,
            period
        );
    }
}

esp_err_t ina226_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task != NULL) {
        return ESP_OK;
    }

    const BaseType_t result =
        xTaskCreatePinnedToCore(
            poll_task,
            "ina226",
            6144,
            NULL,
            4,
            &s_task,
            0
        );

    if (result != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    return ESP_OK;
}

esp_err_t ina226_stop(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    s_running = false;
    return ESP_OK;
}

esp_err_t ina226_get_latest(
    ina226_measurement_t *measurement
)
{
    if (!s_initialized ||
        measurement == NULL ||
        s_data_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    *measurement = s_latest;
    xSemaphoreGive(s_data_mutex);

    return measurement->valid
        ? ESP_OK
        : ESP_ERR_INVALID_STATE;
}

bool ina226_is_initialized(void)
{
    return s_initialized;
}

bool ina226_is_running(void)
{
    return s_running;
}
