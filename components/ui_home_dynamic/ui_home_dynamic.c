#include "ui_home_dynamic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rgb_display.h"

static const char *TAG = "ui_home";

#define UI_W 480
#define UI_H 480

#define ARC_CX 240
#define ARC_CY 240
#define ARC_RADIUS 224
#define ARC_THICKNESS 16
/* Screen coordinates: 90 degrees points downward. This sector is centered at the bottom. */
#define ARC_START_DEG 0
#define ARC_END_DEG 180

#define POWER_X 214
#define POWER_Y 74
#define MINUS_X 67
#define MINUS_Y 238
#define PLUS_X 367
#define PLUS_Y 216
#define MODE_X 185
#define MODE_Y 286

#define TEMP_X 150
#define TEMP_Y 166
#define TEMP_W 185
#define TEMP_H 86

#define RGB565(r, g, b) ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | \
                                    (((uint16_t)(g) & 0xFCU) << 3) | \
                                    ((uint16_t)(b) >> 3)))

#pragma pack(push, 1)
typedef struct {
    char magic[4];
    uint16_t width;
    uint16_t height;
} arle_header_t;

typedef struct {
    uint16_t count;
    uint16_t color;
    uint8_t alpha;
} arle_run_t;

typedef struct {
    uint16_t color;
    uint8_t alpha;
} ui_pixel_t;
#pragma pack(pop)

typedef struct {
    uint16_t width;
    uint16_t height;
    ui_pixel_t *pixels;
} ui_asset_t;

typedef enum {
    BUTTON_NONE = 0,
    BUTTON_POWER,
    BUTTON_MINUS,
    BUTTON_PLUS,
    BUTTON_MODE,
} button_id_t;

static ui_home_config_t s_config;
static ui_home_model_t s_model;
static bool s_initialized;
static button_id_t s_pressed_button;
static SemaphoreHandle_t s_mutex;

static ui_asset_t s_power_off;
static ui_asset_t s_power_on;
static ui_asset_t s_minus_off;
static ui_asset_t s_minus_on;
static ui_asset_t s_plus_off;
static ui_asset_t s_plus_on;
static ui_asset_t s_mode_off;
static ui_asset_t s_mode_on;

static uint16_t *fb(void)
{
    return rgb_display_get_framebuffer();
}

static uint16_t battery_color(uint8_t percent)
{
    /* One uniform color is used for the complete active battery segment. */
    if (percent <= 10) return RGB565(255, 45, 35);   /* critical: red */
    if (percent <= 25) return RGB565(255, 105, 25);  /* low: orange */
    if (percent <= 50) return RGB565(255, 205, 35);  /* medium: yellow */
    if (percent <= 80) return RGB565(70, 220, 105);  /* good: green */
    return RGB565(0, 190, 255);                      /* high: cyan */
}

static uint16_t alpha_blend565(uint16_t dst, uint16_t src, uint8_t alpha)
{
    if (alpha == 255) return src;
    if (alpha == 0) return dst;

    uint32_t sr = (src >> 11) & 0x1F;
    uint32_t sg = (src >> 5) & 0x3F;
    uint32_t sb = src & 0x1F;
    uint32_t dr = (dst >> 11) & 0x1F;
    uint32_t dg = (dst >> 5) & 0x3F;
    uint32_t db = dst & 0x1F;

    uint32_t inv = 255U - alpha;
    uint32_t r = (sr * alpha + dr * inv + 127U) / 255U;
    uint32_t g = (sg * alpha + dg * inv + 127U) / 255U;
    uint32_t b = (sb * alpha + db * inv + 127U) / 255U;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void put_pixel(int x, int y, uint16_t color)
{
    if ((unsigned)x < UI_W && (unsigned)y < UI_H) {
        fb()[y * UI_W + x] = color;
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    if (w <= 0 || h <= 0) return;

    uint16_t *frame = fb();
    for (int yy = 0; yy < h; ++yy) {
        uint16_t *row = frame + (y + yy) * UI_W + x;
        for (int xx = 0; xx < w; ++xx) row[xx] = color;
    }
}

static void fill_circle(int cx, int cy, int radius, uint16_t color)
{
    int rr = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        int xx = (int)sqrtf((float)(rr - y * y));
        fill_rect(cx - xx, cy + y, xx * 2 + 1, 1, color);
    }
}

static void draw_disc(int cx, int cy, int radius, uint16_t color)
{
    fill_circle(cx, cy, radius, color);
}

static void draw_arc_segment(int angle_deg, int radius, int thickness, uint16_t color)
{
    float rad = (float)angle_deg * (float)M_PI / 180.0f;
    int cx = (int)lroundf((float)ARC_CX + cosf(rad) * radius);
    int cy = (int)lroundf((float)ARC_CY + sinf(rad) * radius);
    draw_disc(cx, cy, thickness / 2, color);
}

static void draw_battery_arc(void)
{
    const uint16_t track_color = RGB565(16, 19, 23);

    /* Draw the complete inactive battery track first. */
    for (int angle = ARC_START_DEG; angle <= ARC_END_DEG; ++angle) {
        draw_arc_segment(angle, ARC_RADIUS, ARC_THICKNESS, track_color);
    }

    if (s_model.battery_percent == 0) {
        return;
    }

    const int range = ARC_END_DEG - ARC_START_DEG;
    const int end_angle = ARC_START_DEG +
        (range * s_model.battery_percent) / 100;
    const uint16_t color = battery_color(s_model.battery_percent);

    /* The entire active part has the same color for the current level. */
    for (int angle = ARC_START_DEG; angle <= end_angle; ++angle) {
        draw_arc_segment(angle, ARC_RADIUS, ARC_THICKNESS, color);
    }
}

static esp_err_t asset_load(ui_asset_t *asset, const char *filename)
{
    char path[160];
    int n = snprintf(path, sizeof(path), "%s/%s", s_config.asset_base_path, filename);
    if (n < 0 || n >= (int)sizeof(path)) return ESP_ERR_INVALID_SIZE;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    arle_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || memcmp(hdr.magic, "ARLE", 4) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t count = (size_t)hdr.width * hdr.height;
    ui_pixel_t *pixels = heap_caps_malloc(count * sizeof(ui_pixel_t), MALLOC_CAP_SPIRAM);
    if (!pixels) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t index = 0;
    arle_run_t run;
    while (index < count && fread(&run, sizeof(run), 1, f) == 1) {
        if (run.count == 0 || index + run.count > count) {
            free(pixels);
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }
        for (uint16_t i = 0; i < run.count; ++i) {
            pixels[index].color = run.color;
            pixels[index].alpha = run.alpha;
            ++index;
        }
    }
    fclose(f);

    if (index != count) {
        free(pixels);
        return ESP_ERR_INVALID_SIZE;
    }

    asset->width = hdr.width;
    asset->height = hdr.height;
    asset->pixels = pixels;
    return ESP_OK;
}

static void draw_asset(const ui_asset_t *asset, int x, int y)
{
    if (!asset || !asset->pixels) return;
    uint16_t *frame = fb();
    for (int sy = 0; sy < asset->height; ++sy) {
        int dy = y + sy;
        if ((unsigned)dy >= UI_H) continue;
        for (int sx = 0; sx < asset->width; ++sx) {
            int dx = x + sx;
            if ((unsigned)dx >= UI_W) continue;
            const ui_pixel_t p = asset->pixels[sy * asset->width + sx];
            if (p.alpha == 0) continue;
            uint16_t *dst = &frame[dy * UI_W + dx];
            *dst = alpha_blend565(*dst, p.color, p.alpha);
        }
    }
}

/* Seven-segment layout: A B C D E F G bits 0..6. */
static const uint8_t digit_segments[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66,
    0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void draw_hseg(int x, int y, int length, int t, uint16_t color)
{
    fill_rect(x + t / 2, y, length - t, t, color);
    draw_disc(x + t / 2, y + t / 2, t / 2, color);
    draw_disc(x + length - t / 2, y + t / 2, t / 2, color);
}

static void draw_vseg(int x, int y, int length, int t, uint16_t color)
{
    fill_rect(x, y + t / 2, t, length - t, color);
    draw_disc(x + t / 2, y + t / 2, t / 2, color);
    draw_disc(x + t / 2, y + length - t / 2, t / 2, color);
}

static void draw_digit(int x, int y, int digit, uint16_t color)
{
    const int w = 42, h = 72, t = 7;
    const int half = h / 2;
    uint8_t s = digit_segments[digit % 10];
    if (s & 0x01) draw_hseg(x, y, w, t, color);                 /* A */
    if (s & 0x02) draw_vseg(x + w - t, y, half, t, color);     /* B */
    if (s & 0x04) draw_vseg(x + w - t, y + half, half, t, color); /* C */
    if (s & 0x08) draw_hseg(x, y + h - t, w, t, color);        /* D */
    if (s & 0x10) draw_vseg(x, y + half, half, t, color);      /* E */
    if (s & 0x20) draw_vseg(x, y, half, t, color);             /* F */
    if (s & 0x40) draw_hseg(x, y + half - t / 2, w, t, color); /* G */
}

static void draw_temperature(void)
{
    fill_rect(TEMP_X, TEMP_Y, TEMP_W, TEMP_H, RGB565(0, 0, 0));

    uint16_t value = s_model.target_temperature;
    if (value > 999) value = 999;
    int d0 = value / 100;
    int d1 = (value / 10) % 10;
    int d2 = value % 10;

    uint16_t white = RGB565(248, 248, 248);
    draw_digit(TEMP_X + 0, TEMP_Y + 5, d0, white);
    draw_digit(TEMP_X + 48, TEMP_Y + 5, d1, white);
    draw_digit(TEMP_X + 96, TEMP_Y + 5, d2, white);

    /* Degree symbol. */
    int cx = TEMP_X + 151;
    int cy = TEMP_Y + 13;
    for (int a = 0; a < 360; a += 4) {
        float r = (float)a * (float)M_PI / 180.0f;
        put_pixel(cx + (int)lroundf(cosf(r) * 7),
                  cy + (int)lroundf(sinf(r) * 7), white);
        put_pixel(cx + (int)lroundf(cosf(r) * 6),
                  cy + (int)lroundf(sinf(r) * 6), white);
    }
}

static void clear_asset_area(int x, int y, int w, int h)
{
    fill_rect(x - 3, y - 3, w + 6, h + 6, RGB565(0, 0, 0));
}

static void draw_power_button(void)
{
    clear_asset_area(POWER_X, POWER_Y, 52, 52);
    draw_asset(s_model.power_on ? &s_power_on : &s_power_off, POWER_X, POWER_Y);
}

static void draw_minus_button(void)
{
    clear_asset_area(MINUS_X, MINUS_Y, 34, 9);
    draw_asset(s_pressed_button == BUTTON_MINUS ? &s_minus_on : &s_minus_off,
               MINUS_X, MINUS_Y);
}

static void draw_plus_button(void)
{
    clear_asset_area(PLUS_X, PLUS_Y, 46, 46);
    draw_asset(s_pressed_button == BUTTON_PLUS ? &s_plus_on : &s_plus_off,
               PLUS_X, PLUS_Y);
}

static void draw_mode_button(void)
{
    clear_asset_area(MODE_X, MODE_Y, 111, 111);
    draw_asset(s_model.ai_enabled ? &s_mode_on : &s_mode_off, MODE_X, MODE_Y);
}

static void draw_button(button_id_t button)
{
    switch (button) {
        case BUTTON_POWER:
            draw_power_button();
            break;

        case BUTTON_MINUS:
            draw_minus_button();
            break;

        case BUTTON_PLUS:
            draw_plus_button();
            break;

        case BUTTON_MODE:
            draw_mode_button();
            break;

        case BUTTON_NONE:
        default:
            break;
    }
}

static void draw_static_background(void)
{
    fill_rect(0, 0, UI_W, UI_H, RGB565(0, 0, 0));

    /* Very subtle central body, matching the black-on-black Figma look. */
    fill_circle(240, 240, 203, RGB565(2, 2, 3));
    fill_circle(240, 240, 197, RGB565(0, 0, 0));
}

static bool in_rect(uint16_t x, uint16_t y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static bool in_circle_hit(uint16_t x, uint16_t y, int cx, int cy, int r)
{
    int dx = (int)x - cx;
    int dy = (int)y - cy;
    return dx * dx + dy * dy <= r * r;
}

static button_id_t hit_test(uint16_t x, uint16_t y)
{
    if (in_circle_hit(x, y, 240, 100, 36)) return BUTTON_POWER;
    if (in_rect(x, y, 42, 205, 90, 80)) return BUTTON_MINUS;
    if (in_rect(x, y, 350, 195, 90, 90)) return BUTTON_PLUS;
    if (in_circle_hit(x, y, 240, 342, 65)) return BUTTON_MODE;
    return BUTTON_NONE;
}

static void emit(ui_home_event_t event)
{
    if (s_config.event_cb) s_config.event_cb(event, &s_model, s_config.user_ctx);
}

esp_err_t ui_home_redraw_all(void)
{
    if (!s_initialized || !fb()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    draw_static_background();
    draw_battery_arc();
    draw_temperature();
    draw_power_button();
    draw_minus_button();
    draw_plus_button();
    draw_mode_button();
    esp_err_t err = rgb_display_present();
    xSemaphoreGiveRecursive(s_mutex);
    return err;
}

esp_err_t ui_home_init(const ui_home_config_t *config,
                       const ui_home_model_t *initial_model)
{
    if (!config || !config->asset_base_path || !initial_model || !fb()) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    s_model = *initial_model;
    if (s_model.battery_percent > 100) s_model.battery_percent = 100;
    if (s_model.target_temperature > 999) s_model.target_temperature = 999;

    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    ESP_RETURN_ON_ERROR(asset_load(&s_power_off, "power_off.arle"), TAG, "power_off");
    ESP_RETURN_ON_ERROR(asset_load(&s_power_on, "power_on.arle"), TAG, "power_on");
    ESP_RETURN_ON_ERROR(asset_load(&s_minus_off, "minus_off.arle"), TAG, "minus_off");
    ESP_RETURN_ON_ERROR(asset_load(&s_minus_on, "minus_on.arle"), TAG, "minus_on");
    ESP_RETURN_ON_ERROR(asset_load(&s_plus_off, "plus_off.arle"), TAG, "plus_off");
    ESP_RETURN_ON_ERROR(asset_load(&s_plus_on, "plus_on.arle"), TAG, "plus_on");
    ESP_RETURN_ON_ERROR(asset_load(&s_mode_off, "mode_off.arle"), TAG, "mode_off");
    ESP_RETURN_ON_ERROR(asset_load(&s_mode_on, "mode_on.arle"), TAG, "mode_on");

    s_initialized = true;
    ESP_LOGI(TAG, "Dynamic home UI initialized");
    return ui_home_redraw_all();
}

void ui_home_set_target_temperature(uint16_t value)
{
    if (!s_initialized) return;
    if (value > 999) value = 999;

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    if (s_model.target_temperature != value) {
        s_model.target_temperature = value;
        draw_temperature();
        (void)rgb_display_present();
    }

    xSemaphoreGiveRecursive(s_mutex);
}

void ui_home_set_current_temperature(uint16_t value)
{
    if (!s_initialized) return;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    s_model.current_temperature = value;
    xSemaphoreGiveRecursive(s_mutex);
}

void ui_home_set_battery_percent(uint8_t value)
{
    if (!s_initialized) return;
    if (value > 100) value = 100;

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    if (s_model.battery_percent != value) {
        s_model.battery_percent = value;
        draw_battery_arc();
        (void)rgb_display_present();
    }

    xSemaphoreGiveRecursive(s_mutex);
}

void ui_home_set_power_enabled(bool enabled)
{
    if (!s_initialized) return;

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    if (s_model.power_on != enabled) {
        s_model.power_on = enabled;
        draw_power_button();
        (void)rgb_display_present();
    }

    xSemaphoreGiveRecursive(s_mutex);
}

void ui_home_set_ai_enabled(bool enabled)
{
    if (!s_initialized) return;

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    if (s_model.ai_enabled != enabled) {
        s_model.ai_enabled = enabled;
        draw_mode_button();
        (void)rgb_display_present();
    }

    xSemaphoreGiveRecursive(s_mutex);
}

void ui_home_set_running(bool value)
{
    if (!s_initialized) return;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    s_model.running = value;
    xSemaphoreGiveRecursive(s_mutex);
}

void ui_home_set_paused(bool value)
{
    if (!s_initialized) return;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    s_model.paused = value;
    xSemaphoreGiveRecursive(s_mutex);
}

ui_home_model_t ui_home_get_model(void)
{
    ui_home_model_t copy = {0};
    if (!s_initialized) return copy;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    copy = s_model;
    xSemaphoreGiveRecursive(s_mutex);
    return copy;
}

esp_err_t ui_home_handle_touch(uint16_t x, uint16_t y, bool pressed)
{
    if (!s_initialized || x >= UI_W || y >= UI_H) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    const button_id_t hit = hit_test(x, y);

    if (pressed) {
        if (hit != s_pressed_button) {
            const button_id_t old_button = s_pressed_button;
            s_pressed_button = hit;

            /*
             * Redraw only buttons whose pressed state changed,
             * then present once.
             */
            draw_button(old_button);
            draw_button(hit);

            const esp_err_t err = rgb_display_present();
            xSemaphoreGiveRecursive(s_mutex);
            return err;
        }

        xSemaphoreGiveRecursive(s_mutex);
        return ESP_OK;
    }

    const button_id_t released = s_pressed_button;
    s_pressed_button = BUTTON_NONE;

    bool changed = false;

    /*
     * Restore the released button from its pressed asset.
     */
    if (released != BUTTON_NONE) {
        draw_button(released);
        changed = true;
    }

    if (released != BUTTON_NONE && released == hit) {
        switch (released) {
            case BUTTON_POWER:
                s_model.power_on = !s_model.power_on;
                draw_power_button();
                emit(UI_HOME_EVENT_POWER_TOGGLED);
                changed = true;
                break;

            case BUTTON_PLUS:
                if (s_model.target_temperature < 999) {
                    ++s_model.target_temperature;
                    draw_temperature();
                }
                draw_plus_button();
                emit(UI_HOME_EVENT_PLUS);
                changed = true;
                break;

            case BUTTON_MINUS:
                if (s_model.target_temperature > 0) {
                    --s_model.target_temperature;
                    draw_temperature();
                }
                draw_minus_button();
                emit(UI_HOME_EVENT_MINUS);
                changed = true;
                break;

            case BUTTON_MODE:
                s_model.ai_enabled = !s_model.ai_enabled;
                draw_mode_button();
                emit(UI_HOME_EVENT_AI_TOGGLED);
                changed = true;
                break;

            case BUTTON_NONE:
            default:
                break;
        }
    }

    esp_err_t err = ESP_OK;
    if (changed) {
        err = rgb_display_present();
    }

    xSemaphoreGiveRecursive(s_mutex);
    return err;
}