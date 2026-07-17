#include "intro_animation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "rgb_display.h"

static const char *TAG = "intro_animation";
static volatile bool s_stop_requested;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t width;
    uint16_t height;
} rle_header_t;

typedef struct __attribute__((packed)) {
    uint16_t count;
    uint16_t color;
} rle_run_t;

esp_err_t intro_animation_mount_spiffs(void)
{
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "SPIFFS mount failed");

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_spiffs_info("storage", &total, &used), TAG, "SPIFFS info failed");
    ESP_LOGI(TAG, "SPIFFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    return ESP_OK;
}

static uint16_t *s_source_frame = NULL;

static esp_err_t draw_rle_frame(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    rle_header_t header;

    if (fread(&header, sizeof(header), 1, file) != 1) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(header.magic, "IRLE", 4) != 0 ||
        header.width == 0 ||
        header.height == 0) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t source_pixels =
        (uint32_t)header.width * header.height;

    if (s_source_frame == NULL) {
        s_source_frame = heap_caps_malloc(
            source_pixels * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM
        );

        if (s_source_frame == NULL) {
            fclose(file);
            return ESP_ERR_NO_MEM;
        }
    }

    uint32_t source_index = 0;
    rle_run_t run;

    while (source_index < source_pixels &&
           fread(&run, sizeof(run), 1, file) == 1) {

        if (run.count == 0 ||
            source_index + run.count > source_pixels) {
            fclose(file);
            return ESP_ERR_INVALID_SIZE;
        }

        for (uint16_t i = 0; i < run.count; ++i) {
            s_source_frame[source_index++] = run.color;
        }
    }

    fclose(file);

    if (source_index != source_pixels) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t *framebuffer = rgb_display_get_framebuffer();

    const int dst_w = rgb_display_get_width();
    const int dst_h = rgb_display_get_height();

    if (framebuffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const int scale_x = dst_w / header.width;
    const int scale_y = dst_h / header.height;

    if (scale_x != 2 || scale_y != 2) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /*
     * 240x240 -> 480x480.
     * Каждая исходная строка записывается в две строки дисплея.
     */
    for (uint32_t sy = 0; sy < header.height; ++sy) {
        const uint16_t *src_row =
            s_source_frame + sy * header.width;

        uint16_t *dst_row_0 =
            framebuffer + (sy * 2) * dst_w;

        uint16_t *dst_row_1 =
            dst_row_0 + dst_w;

        for (uint32_t sx = 0; sx < header.width; ++sx) {
            const uint16_t color = src_row[sx];
            const uint32_t dx = sx * 2;

            dst_row_0[dx] = color;
            dst_row_0[dx + 1] = color;

            dst_row_1[dx] = color;
            dst_row_1[dx + 1] = color;
        }
    }

    /* Present exactly once after the complete frame is ready. */
    return rgb_display_present();
}

esp_err_t intro_animation_play(const intro_animation_config_t *config)
{
    if (config == NULL ||
        config->base_path == NULL ||
        config->frame_count == 0 ||
        config->frame_period_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_stop_requested = false;

    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t frame_period =
        pdMS_TO_TICKS(config->frame_period_ms);

    do {
        for (uint32_t frame = 0; frame < config->frame_count; ++frame) {
            if (s_stop_requested) {
                ESP_LOGI(TAG, "Animation stop requested");
                return ESP_OK;
            }

            char path[96];

            snprintf(
                path,
                sizeof(path),
                "%s/frame_%03lu.rle",
                config->base_path,
                (unsigned long)frame
            );

            esp_err_t err = draw_rle_frame(path);
            if (err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Frame %lu failed: %s",
                    (unsigned long)frame,
                    esp_err_to_name(err)
                );
                return err;
            }

            /*
             * Даём IDLE-задаче и другим задачам время выполнения.
             * При 20 FPS период равен 50 мс.
             */
            vTaskDelayUntil(&last_wake_time, frame_period);
        }
    } while (config->loop);

    return ESP_OK;
}

void intro_animation_request_stop(void)
{
    s_stop_requested = true;
}
