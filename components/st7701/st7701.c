#include "st7701.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_pins.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "st7701";

#define ST7701_SPI_HOST       SPI2_HOST
#define ST7701_SPI_FREQ_HZ    10000000

static spi_device_handle_t s_spi = NULL;
static bool s_initialized = false;

static esp_err_t st7701_write_sequence(uint8_t command, const uint8_t *data, size_t data_len)
{
    ESP_RETURN_ON_ERROR(st7701_write_command(command), TAG, "Command 0x%02X failed", command);

    for (size_t i = 0; i < data_len; ++i) {
        ESP_RETURN_ON_ERROR(st7701_write_data(data[i]), TAG,
                            "Data 0x%02X for command 0x%02X failed", data[i], command);
    }

    return ESP_OK;
}

esp_err_t st7701_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num = BOARD_GPIO_LCD_SDA,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_GPIO_LCD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = 16,
    };

    esp_err_t err = spi_bus_initialize(ST7701_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    const spi_device_interface_config_t device_config = {
        .command_bits = 1,
        .address_bits = 8,
        .dummy_bits = 0,
        .mode = 0,
        .clock_speed_hz = ST7701_SPI_FREQ_HZ,
        .spics_io_num = BOARD_GPIO_LCD_CS,
        .queue_size = 1,
    };

    err = spi_bus_add_device(ST7701_SPI_HOST, &device_config, &s_spi);
    if (err != ESP_OK) {
        if (err != ESP_ERR_INVALID_STATE) {
            spi_bus_free(ST7701_SPI_HOST);
        }
        ESP_LOGE(TAG, "Failed to add ST7701S SPI device: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "3-wire SPI initialized: SDA=%d SCK=%d CS=%d",
             BOARD_GPIO_LCD_SDA, BOARD_GPIO_LCD_SCK, BOARD_GPIO_LCD_CS);

    return ESP_OK;
}

esp_err_t st7701_write_command(uint8_t command)
{
    if (!s_initialized || s_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_transaction_t transaction = {
        .cmd = 0,
        .addr = command,
        .length = 0,
        .rxlength = 0,
    };

    return spi_device_transmit(s_spi, &transaction);
}

esp_err_t st7701_write_data(uint8_t data)
{
    if (!s_initialized || s_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_transaction_t transaction = {
        .cmd = 1,
        .addr = data,
        .length = 0,
        .rxlength = 0,
    };

    return spi_device_transmit(s_spi, &transaction);
}

esp_err_t st7701_panel_init(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    static const uint8_t ff_13[] = {0x77, 0x01, 0x00, 0x00, 0x13};
    static const uint8_t ef_08[] = {0x08};
    static const uint8_t ff_10[] = {0x77, 0x01, 0x00, 0x00, 0x10};
    static const uint8_t c0[] = {0x3B, 0x00};
    static const uint8_t c1[] = {0x10, 0x0C};
    static const uint8_t c2[] = {0x07, 0x0A};
    static const uint8_t c7[] = {0x00};
    static const uint8_t cc[] = {0x10};
    static const uint8_t cd[] = {0x08};
    static const uint8_t b0_gamma[] = {
        0x05, 0x12, 0x98, 0x0E, 0x0F, 0x07, 0x07, 0x09,
        0x09, 0x23, 0x05, 0x52, 0x0F, 0x67, 0x2C, 0x11
    };
    static const uint8_t b1_gamma[] = {
        0x0B, 0x11, 0x97, 0x0C, 0x12, 0x06, 0x06, 0x08,
        0x08, 0x22, 0x03, 0x51, 0x11, 0x66, 0x2B, 0x0F
    };
    static const uint8_t ff_11[] = {0x77, 0x01, 0x00, 0x00, 0x11};
    static const uint8_t b0[] = {0x5D};
    static const uint8_t b1[] = {0x3E};
    static const uint8_t b2[] = {0x81};
    static const uint8_t b3[] = {0x80};
    static const uint8_t b5[] = {0x4E};
    static const uint8_t b7[] = {0x85};
    static const uint8_t b8[] = {0x20};
    static const uint8_t c1_11[] = {0x78};
    static const uint8_t c2_11[] = {0x78};
    static const uint8_t d0[] = {0x88};
    static const uint8_t e0[] = {0x00, 0x00, 0x02};
    static const uint8_t e1[] = {
        0x06, 0x30, 0x08, 0x30, 0x05, 0x30, 0x07, 0x30,
        0x00, 0x33, 0x33
    };
    static const uint8_t e2[] = {
        0x11, 0x11, 0x33, 0x33, 0xF4, 0x00,
        0x00, 0x00, 0xF4, 0x00, 0x00, 0x00
    };
    static const uint8_t e3[] = {0x00, 0x00, 0x11, 0x11};
    static const uint8_t e4[] = {0x44, 0x44};
    static const uint8_t e5[] = {
        0x0D, 0xF5, 0x30, 0xF0, 0x0F, 0xF7, 0x30, 0xF0,
        0x09, 0xF1, 0x30, 0xF0, 0x0B, 0xF3, 0x30, 0xF0
    };
    static const uint8_t e6[] = {0x00, 0x00, 0x11, 0x11};
    static const uint8_t e7[] = {0x44, 0x44};
    static const uint8_t e8[] = {
        0x0C, 0xF4, 0x30, 0xF0, 0x0E, 0xF6, 0x30, 0xF0,
        0x08, 0xF0, 0x30, 0xF0, 0x0A, 0xF2, 0x30, 0xF0
    };
    static const uint8_t e9[] = {0x36, 0x01};
    static const uint8_t eb[] = {0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40};
    static const uint8_t ed[] = {
        0xFF, 0x10, 0xAF, 0x76, 0x54, 0x2B, 0xCF, 0xFF,
        0xFF, 0xFC, 0xB2, 0x45, 0x67, 0xFA, 0x01, 0xFF
    };
    static const uint8_t ef[] = {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54};
    static const uint8_t ff_00[] = {0x77, 0x01, 0x00, 0x00, 0x00};
    static const uint8_t pixel_format[] = {0x66}; // 16-bit/pixel (RGB565)
    static const uint8_t madctl[] = {0x00};
    static const uint8_t tearing[] = {0x00};

    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xFF, ff_13, sizeof(ff_13)), TAG, "FF page 13 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xEF, ef_08, sizeof(ef_08)), TAG, "EF failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xFF, ff_10, sizeof(ff_10)), TAG, "FF page 10 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xC0, c0, sizeof(c0)), TAG, "C0 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xC1, c1, sizeof(c1)), TAG, "C1 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xC2, c2, sizeof(c2)), TAG, "C2 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xC7, c7, sizeof(c7)), TAG, "C7 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xCC, cc, sizeof(cc)), TAG, "CC failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xCD, cd, sizeof(cd)), TAG, "CD failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xB0, b0_gamma, sizeof(b0_gamma)), TAG, "B0 gamma failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xB1, b1_gamma, sizeof(b1_gamma)), TAG, "B1 gamma failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xFF, ff_11, sizeof(ff_11)), TAG, "FF page 11 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xB0, b0, sizeof(b0)), TAG, "B0 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xB1, b1, sizeof(b1)), TAG, "B1 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xB2, b2, sizeof(b2)), TAG, "B2 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xB3, b3, sizeof(b3)), TAG, "B3 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xB5, b5, sizeof(b5)), TAG, "B5 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xB7, b7, sizeof(b7)), TAG, "B7 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xB8, b8, sizeof(b8)), TAG, "B8 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xC1, c1_11, sizeof(c1_11)), TAG, "C1 page 11 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xC2, c2_11, sizeof(c2_11)), TAG, "C2 page 11 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xD0, d0, sizeof(d0)), TAG, "D0 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xE0, e0, sizeof(e0)), TAG, "E0 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xE1, e1, sizeof(e1)), TAG, "E1 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xE2, e2, sizeof(e2)), TAG, "E2 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xE3, e3, sizeof(e3)), TAG, "E3 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xE4, e4, sizeof(e4)), TAG, "E4 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xE5, e5, sizeof(e5)), TAG, "E5 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xE6, e6, sizeof(e6)), TAG, "E6 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xE7, e7, sizeof(e7)), TAG, "E7 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xE8, e8, sizeof(e8)), TAG, "E8 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xE9, e9, sizeof(e9)), TAG, "E9 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xEB, eb, sizeof(eb)), TAG, "EB failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xED, ed, sizeof(ed)), TAG, "ED failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xEF, ef, sizeof(ef)), TAG, "EF page 11 failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0xFF, ff_00, sizeof(ff_00)), TAG, "FF page 00 failed");

    ESP_RETURN_ON_ERROR(st7701_write_command(0x11), TAG, "Sleep out failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(st7701_write_sequence(0x3A, pixel_format, sizeof(pixel_format)), TAG, "Pixel format failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0x36, madctl, sizeof(madctl)), TAG, "MADCTL failed");
    ESP_RETURN_ON_ERROR(st7701_write_sequence(0x35, tearing, sizeof(tearing)), TAG, "TE failed");
    ESP_RETURN_ON_ERROR(st7701_write_command(0x20), TAG, "Normal display mode failed");

    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(st7701_write_command(0x29), TAG, "Display on failed");

    ESP_LOGI(TAG, "Panel initialization complete");
    return ESP_OK;
}
