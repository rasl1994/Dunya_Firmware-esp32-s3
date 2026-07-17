#include "rgb_display.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_pins.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "rgb_display";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_framebuffers[2];

static uint8_t s_front_index;
static uint8_t s_back_index;

static SemaphoreHandle_t s_render_mutex;
static SemaphoreHandle_t s_vsync_sem;

static size_t frame_bytes(void)
{
    return (size_t)BOARD_LCD_H_RES *
           (size_t)BOARD_LCD_V_RES *
           sizeof(uint16_t);
}

static bool IRAM_ATTR rgb_display_on_vsync(
    esp_lcd_panel_handle_t panel,
    const esp_lcd_rgb_panel_event_data_t *event_data,
    void *user_ctx
)
{
    (void)panel;
    (void)event_data;
    (void)user_ctx;

    BaseType_t high_task_wakeup = pdFALSE;

    if (s_vsync_sem != NULL) {
        xSemaphoreGiveFromISR(
            s_vsync_sem,
            &high_task_wakeup
        );
    }

    return high_task_wakeup == pdTRUE;
}

esp_err_t rgb_display_init(void)
{
    if (s_panel != NULL) {
        return ESP_OK;
    }

    s_render_mutex = xSemaphoreCreateMutex();
    s_vsync_sem = xSemaphoreCreateBinary();

    if (s_render_mutex == NULL || s_vsync_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,

        .timings = {
            .pclk_hz = 12 * 1000 * 1000,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,

            .hsync_pulse_width = 10,
            .hsync_back_porch = 20,
            .hsync_front_porch = 10,

            .vsync_pulse_width = 2,
            .vsync_back_porch = 18,
            .vsync_front_porch = 10,

            .flags = {
                .hsync_idle_low = false,
                .vsync_idle_low = false,
                .de_idle_high = false,
                .pclk_active_neg = true,
                .pclk_idle_high = false,
            },
        },

        .data_width = 16,
        .num_fbs = 2,

        /*
         * Full framebuffers are used. Keeping bounce buffer disabled avoids
         * another intermediate buffer and preserves the configuration that
         * already compiled with the user's ESP-IDF version.
         */
        .bounce_buffer_size_px = BOARD_LCD_H_RES * 20,

        .hsync_gpio_num = BOARD_GPIO_LCD_HSYNC,
        .vsync_gpio_num = BOARD_GPIO_LCD_VSYNC,
        .de_gpio_num = BOARD_GPIO_LCD_DE,
        .pclk_gpio_num = BOARD_GPIO_LCD_PCLK,
        .disp_gpio_num = GPIO_NUM_NC,

        .data_gpio_nums = {
            BOARD_GPIO_LCD_B1,
            BOARD_GPIO_LCD_B2,
            BOARD_GPIO_LCD_B3,
            BOARD_GPIO_LCD_B4,
            BOARD_GPIO_LCD_B5,

            BOARD_GPIO_LCD_G0,
            BOARD_GPIO_LCD_G1,
            BOARD_GPIO_LCD_G2,
            BOARD_GPIO_LCD_G3,
            BOARD_GPIO_LCD_G4,
            BOARD_GPIO_LCD_G5,

            BOARD_GPIO_LCD_R1,
            BOARD_GPIO_LCD_R2,
            BOARD_GPIO_LCD_R3,
            BOARD_GPIO_LCD_R4,
            BOARD_GPIO_LCD_R5,
        },

        .flags = {
            .disp_active_low = false,
            .refresh_on_demand = false,
            .fb_in_psram = true,
            .no_fb = false,
            .bb_invalidate_cache = false,
        },
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_rgb_panel(&panel_config, &s_panel),
        TAG,
        "Failed to create RGB panel"
    );

    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_vsync = rgb_display_on_vsync,
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_rgb_panel_register_event_callbacks(
            s_panel,
            &callbacks,
            NULL
        ),
        TAG,
        "Failed to register RGB callbacks"
    );

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_reset(s_panel),
        TAG,
        "RGB panel reset failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_init(s_panel),
        TAG,
        "RGB panel init failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_lcd_rgb_panel_get_frame_buffer(
            s_panel,
            2,
            (void **)&s_framebuffers[0],
            (void **)&s_framebuffers[1]
        ),
        TAG,
        "Unable to get RGB framebuffers"
    );

    if (s_framebuffers[0] == NULL ||
        s_framebuffers[1] == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(s_framebuffers[0], 0, frame_bytes());
    memset(s_framebuffers[1], 0, frame_bytes());

    /*
     * The driver starts with the first framebuffer. The application renders
     * the next frame into the second one.
     */
    s_front_index = 0;
    s_back_index = 1;

    ESP_LOGI(
        TAG,
        "RGB VSYNC double buffering enabled: front=%p back=%p",
        s_framebuffers[s_front_index],
        s_framebuffers[s_back_index]
    );

    return ESP_OK;
}

uint16_t *rgb_display_get_framebuffer(void)
{
    if (s_panel == NULL) {
        return NULL;
    }

    return s_framebuffers[s_back_index];
}

esp_err_t rgb_display_present(void)
{
    if (s_panel == NULL ||
        s_framebuffers[s_back_index] == NULL ||
        s_render_mutex == NULL ||
        s_vsync_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            s_render_mutex,
            portMAX_DELAY
        ) != pdTRUE) {
        return ESP_FAIL;
    }

    /*
     * Remove an old VSYNC event. The event we wait for below must occur
     * after esp_lcd_panel_draw_bitmap() requests the framebuffer switch.
     */
    while (xSemaphoreTake(s_vsync_sem, 0) == pdTRUE) {
    }

    const uint8_t requested_index = s_back_index;

    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel,
        0,
        0,
        BOARD_LCD_H_RES,
        BOARD_LCD_V_RES,
        s_framebuffers[requested_index]
    );

    if (err != ESP_OK) {
        xSemaphoreGive(s_render_mutex);
        return err;
    }

    /*
     * Do not allow the next frame to overwrite a buffer until the RGB
     * peripheral has crossed VSYNC and accepted the requested framebuffer.
     */
    if (xSemaphoreTake(
            s_vsync_sem,
            pdMS_TO_TICKS(100)
        ) != pdTRUE) {
        ESP_LOGE(TAG, "VSYNC framebuffer switch timeout");
        xSemaphoreGive(s_render_mutex);
        return ESP_ERR_TIMEOUT;
    }

    s_front_index = requested_index;
    s_back_index = requested_index ^ 1U;

    /*
     * ui_home_dynamic redraws only changed regions. Synchronize the new back
     * buffer with the frame currently visible on the display before allowing
     * the next partial update.
     */
    memcpy(
        s_framebuffers[s_back_index],
        s_framebuffers[s_front_index],
        frame_bytes()
    );

    xSemaphoreGive(s_render_mutex);
    return ESP_OK;
}

esp_err_t rgb_display_fill(uint16_t color)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t pixel_count =
        (size_t)BOARD_LCD_H_RES *
        (size_t)BOARD_LCD_V_RES;

    for (int buffer = 0; buffer < 2; ++buffer) {
        for (size_t i = 0; i < pixel_count; ++i) {
            s_framebuffers[buffer][i] = color;
        }
    }

    return rgb_display_present();
}

esp_err_t rgb_display_draw_bitmap(
    int x,
    int y,
    int width,
    int height,
    const uint16_t *pixels
)
{
    if (s_panel == NULL || pixels == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (x < 0 ||
        y < 0 ||
        width <= 0 ||
        height <= 0 ||
        x + width > BOARD_LCD_H_RES ||
        y + height > BOARD_LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t *back = rgb_display_get_framebuffer();
    if (back == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int row = 0; row < height; ++row) {
        memcpy(
            back +
                (size_t)(y + row) *
                BOARD_LCD_H_RES +
                x,

            pixels +
                (size_t)row *
                width,

            (size_t)width *
                sizeof(uint16_t)
        );
    }

    return ESP_OK;
}

int rgb_display_get_width(void)
{
    return BOARD_LCD_H_RES;
}

int rgb_display_get_height(void)
{
    return BOARD_LCD_V_RES;
}

esp_lcd_panel_handle_t rgb_display_get_panel(void)
{
    return s_panel;
}
