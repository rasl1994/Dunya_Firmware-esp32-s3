#include "service_screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rgb_display.h"
#include "MAX6675.h"

#define UI_W 480
#define UI_H 480

#define UI_CX 240
#define UI_CY 240
#define UI_VISIBLE_RADIUS 236

#define BTN_BACK_X 190
#define BTN_BACK_Y 18
#define BTN_BACK_W 100
#define BTN_BACK_H 40

#define BTN_MINUS_X 86
#define BTN_MINUS_Y 362
#define BTN_MINUS_W 84
#define BTN_MINUS_H 56

#define BTN_ENABLE_X 178
#define BTN_ENABLE_Y 358
#define BTN_ENABLE_W 124
#define BTN_ENABLE_H 64

#define BTN_PLUS_X 310
#define BTN_PLUS_Y 362
#define BTN_PLUS_W 84
#define BTN_PLUS_H 56

#define RGB565(r, g, b) ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | \
                                    (((uint16_t)(g) & 0xFCU) << 3) | \
                                    ((uint16_t)(b) >> 3)))

typedef enum {
    SERVICE_BUTTON_NONE = 0,
    SERVICE_BUTTON_BACK,
    SERVICE_BUTTON_MINUS,
    SERVICE_BUTTON_PLUS,
    SERVICE_BUTTON_ENABLE,
} service_button_t;

static const char *TAG = "service_screen";

static service_screen_config_t s_config;
static service_screen_model_t s_model;
static SemaphoreHandle_t s_mutex;
static bool s_initialized;
static bool s_visible;
static service_button_t s_pressed_button;

static uint16_t *fb(void)
{
    return rgb_display_get_framebuffer();
}

static void put_pixel(int x, int y, uint16_t color)
{
    uint16_t *frame = fb();
    if (frame != NULL &&
        (unsigned)x < UI_W &&
        (unsigned)y < UI_H) {
        frame[y * UI_W + x] = color;
    }
}

static void fill_rect(
    int x,
    int y,
    int w,
    int h,
    uint16_t color
)
{
    if (x < 0) {
        w += x;
        x = 0;
    }

    if (y < 0) {
        h += y;
        y = 0;
    }

    if (x + w > UI_W) {
        w = UI_W - x;
    }

    if (y + h > UI_H) {
        h = UI_H - y;
    }

    if (w <= 0 || h <= 0) {
        return;
    }

    uint16_t *frame = fb();
    if (frame == NULL) {
        return;
    }

    for (int yy = 0; yy < h; ++yy) {
        uint16_t *row =
            frame +
            (size_t)(y + yy) * UI_W +
            x;

        for (int xx = 0; xx < w; ++xx) {
            row[xx] = color;
        }
    }
}

static void fill_circle(
    int cx,
    int cy,
    int radius,
    uint16_t color
)
{
    const int rr = radius * radius;

    for (int y = -radius; y <= radius; ++y) {
        const int xx =
            (int)sqrtf((float)(rr - y * y));

        fill_rect(
            cx - xx,
            cy + y,
            xx * 2 + 1,
            1,
            color
        );
    }
}

static void draw_ring(
    int cx,
    int cy,
    int outer_radius,
    int thickness,
    uint16_t color
)
{
    fill_circle(cx, cy, outer_radius, color);
    fill_circle(
        cx,
        cy,
        outer_radius - thickness,
        RGB565(0, 0, 0)
    );
}

/*
 * Compact 5x7 font for the labels used on the service screen.
 */
typedef struct {
    char character;
    uint8_t rows[7];
} glyph_t;

static const glyph_t s_font[] = {
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3',{0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5',{0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}},
    {'6',{0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}},
    {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9',{0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}},
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D',{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
    {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'%',{0x11,0x02,0x04,0x08,0x10,0x11,0x00}},
    {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'+',{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
};

static const uint8_t *find_glyph(char c)
{
    for (size_t i = 0;
         i < sizeof(s_font) / sizeof(s_font[0]);
         ++i) {
        if (s_font[i].character == c) {
            return s_font[i].rows;
        }
    }

    return NULL;
}

static void draw_char(
    int x,
    int y,
    char c,
    int scale,
    uint16_t color
)
{
    const uint8_t *rows = find_glyph(c);
    if (rows == NULL) {
        return;
    }

    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (rows[row] & (1U << (4 - col))) {
                fill_rect(
                    x + col * scale,
                    y + row * scale,
                    scale,
                    scale,
                    color
                );
            }
        }
    }
}

static int text_width(const char *text, int scale)
{
    if (text == NULL) {
        return 0;
    }

    return (int)strlen(text) * 6 * scale - scale;
}

static void draw_text(
    int x,
    int y,
    const char *text,
    int scale,
    uint16_t color
)
{
    if (text == NULL) {
        return;
    }

    while (*text != '\0') {
        draw_char(x, y, *text, scale, color);
        x += 6 * scale;
        ++text;
    }
}

static void draw_text_centered(
    int center_x,
    int y,
    const char *text,
    int scale,
    uint16_t color
)
{
    draw_text(
        center_x - text_width(text, scale) / 2,
        y,
        text,
        scale,
        color
    );
}

static uint16_t load_color(uint8_t percent)
{
    if (!s_model.load_enabled) {
        return RGB565(65, 70, 76);
    }

    if (percent <= 25) {
        return RGB565(0, 190, 255);
    }

    if (percent <= 60) {
        return RGB565(70, 220, 105);
    }

    if (percent <= 80) {
        return RGB565(255, 205, 35);
    }

    return RGB565(255, 80, 35);
}

static void draw_measurements(void)
{
    /*
     * Compact INA226 panel on the right side.
     * The heater gauge remains centered at x=240.
     *
     * The panel is intentionally narrow so it does not cover the
     * percentage value in the middle of the gauge.
     */
    const int panel_x = 360;
    const int panel_y = 176;
    const int panel_w = 84;
    const int panel_h = 168;

    fill_rect(
        panel_x,
        panel_y,
        panel_w,
        panel_h,
        RGB565(0, 0, 0)
    );

    char voltage[16];
    char current[16];
    char power[16];
    char temperature[16];

    if (s_model.measurement_valid) {
        const float display_current =
            fabsf(s_model.current_a) < 0.0005f
                ? 0.0f
                : s_model.current_a;

        const float display_power =
            fabsf(s_model.power_w) < 0.01f
                ? 0.0f
                : s_model.power_w;

        snprintf(
            voltage,
            sizeof(voltage),
            "%.1fV",
            (double)s_model.voltage_v
        );

        snprintf(
            current,
            sizeof(current),
            "%.2fA",
            (double)display_current
        );

        snprintf(
            power,
            sizeof(power),
            "%.1fW",
            (double)display_power
        );
    } else {
        snprintf(voltage, sizeof(voltage), "--.-V");
        snprintf(current, sizeof(current), "--.--A");
        snprintf(power, sizeof(power), "---.-W");
    }

    if (s_model.temperature_valid &&
        isfinite(s_model.temperature_c)) {
        snprintf(
            temperature,
            sizeof(temperature),
            "%.1fC",
            (double)s_model.temperature_c
        );
    } else {
        snprintf(
            temperature,
            sizeof(temperature),
            "--.-C"
        );
    }

    draw_text_centered(
        402,
        184,
        voltage,
        2,
        RGB565(0, 190, 255)
    );

    draw_text_centered(
        402,
        224,
        current,
        2,
        RGB565(70, 220, 105)
    );

    draw_text_centered(
        402,
        264,
        power,
        2,
        RGB565(255, 205, 35)
    );

    draw_text_centered(
        402,
        304,
        temperature,
        2,
        RGB565(255, 105, 70)
    );
}

static void draw_load_gauge(void)
{
    /*
     * Heater power indicator remains centered on the display.
     */
    const int cx = 240;
    const int cy = 228;
    const int radius = 118;
    const int thickness = 16;

    draw_ring(
        cx,
        cy,
        radius,
        thickness,
        RGB565(30, 34, 39)
    );

    if (s_model.load_percent > 0) {
        const int start_angle = 135;
        const int range = 270;
        const int filled =
            (range * s_model.load_percent) / 100;

        const uint16_t color =
            load_color(s_model.load_percent);

        for (int angle = 0; angle <= filled; ++angle) {
            const float rad =
                (float)(start_angle + angle) *
                (float)M_PI / 180.0f;

            const int x =
                (int)lroundf(
                    cx +
                    cosf(rad) *
                    (radius - thickness / 2)
                );

            const int y =
                (int)lroundf(
                    cy +
                    sinf(rad) *
                    (radius - thickness / 2)
                );

            fill_circle(
                x,
                y,
                thickness / 2,
                color
            );
        }
    }

    char value[8];

    snprintf(
        value,
        sizeof(value),
        "%u%%",
        (unsigned)s_model.load_percent
    );

    draw_text_centered(
        cx,
        166,
        value,
        5,
        RGB565(248, 248, 248)
    );

    draw_measurements();
}

static void draw_button_box(
    int x,
    int y,
    int w,
    int h,
    bool pressed,
    bool active,
    const char *label
)
{
    uint16_t border = active
        ? RGB565(0, 190, 255)
        : RGB565(80, 86, 94);

    uint16_t fill = pressed
        ? RGB565(28, 34, 41)
        : RGB565(10, 12, 15);

    fill_rect(x, y, w, h, border);
    fill_rect(x + 3, y + 3, w - 6, h - 6, fill);

    draw_text_centered(
        x + w / 2,
        y + (h - 14) / 2,
        label,
        2,
        RGB565(242, 242, 242)
    );
}

static void draw_screen(void)
{
    fill_rect(0, 0, UI_W, UI_H, RGB565(0, 0, 0));

    draw_button_box(BTN_BACK_X, BTN_BACK_Y, BTN_BACK_W, BTN_BACK_H,
                    s_pressed_button == SERVICE_BUTTON_BACK, false, "BACK");

    draw_text_centered(240, 70, "SERVICE", 3, RGB565(242, 242, 242));
    draw_text_centered(240, 98, "LOAD CONTROL", 2, RGB565(120, 130, 140));

    draw_load_gauge();

    draw_button_box(BTN_MINUS_X, BTN_MINUS_Y, BTN_MINUS_W, BTN_MINUS_H,
                    s_pressed_button == SERVICE_BUTTON_MINUS, false, "-");
    draw_button_box(BTN_ENABLE_X, BTN_ENABLE_Y, BTN_ENABLE_W, BTN_ENABLE_H,
                    s_pressed_button == SERVICE_BUTTON_ENABLE,
                    s_model.load_enabled, s_model.load_enabled ? "ON" : "OFF");
    draw_button_box(BTN_PLUS_X, BTN_PLUS_Y, BTN_PLUS_W, BTN_PLUS_H,
                    s_pressed_button == SERVICE_BUTTON_PLUS, false, "+");
}

static bool in_rect(
    uint16_t x,
    uint16_t y,
    int rx,
    int ry,
    int rw,
    int rh
)
{
    return x >= rx &&
           x < rx + rw &&
           y >= ry &&
           y < ry + rh;
}

static bool inside_round_display(
    uint16_t x,
    uint16_t y
)
{
    const int dx = (int)x - UI_CX;
    const int dy = (int)y - UI_CY;

    return dx * dx + dy * dy <=
           UI_VISIBLE_RADIUS * UI_VISIBLE_RADIUS;
}

static service_button_t hit_test(
    uint16_t x,
    uint16_t y
)
{
    if (!inside_round_display(x, y)) {
        return SERVICE_BUTTON_NONE;
    }

    if (in_rect(x, y, BTN_BACK_X, BTN_BACK_Y, BTN_BACK_W, BTN_BACK_H)) {
        return SERVICE_BUTTON_BACK;
    }
    if (in_rect(x, y, BTN_MINUS_X, BTN_MINUS_Y, BTN_MINUS_W, BTN_MINUS_H)) {
        return SERVICE_BUTTON_MINUS;
    }
    if (in_rect(x, y, BTN_PLUS_X, BTN_PLUS_Y, BTN_PLUS_W, BTN_PLUS_H)) {
        return SERVICE_BUTTON_PLUS;
    }
    if (in_rect(x, y, BTN_ENABLE_X, BTN_ENABLE_Y, BTN_ENABLE_W, BTN_ENABLE_H)) {
        return SERVICE_BUTTON_ENABLE;
    }

    return SERVICE_BUTTON_NONE;
}

static void emit(service_screen_event_t event)
{
    if (s_config.event_cb != NULL) {
        s_config.event_cb(
            event,
            &s_model,
            s_config.user_ctx
        );
    }
}

static void service_temperature_handler(
    bool valid,
    float temperature_c,
    void *user_ctx
)
{
    (void)user_ctx;

    service_screen_set_temperature(
        valid,
        temperature_c
    );
}

esp_err_t service_screen_init(
    const service_screen_config_t *config,
    const service_screen_model_t *initial_model
)
{
    if (config == NULL ||
        initial_model == NULL ||
        fb() == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_config = *config;
    s_model = *initial_model;

    if (s_model.load_percent > 100) {
        s_model.load_percent = 100;
    }

    s_model.temperature_valid = false;
    s_model.temperature_c = 0.0f;

    s_pressed_button = SERVICE_BUTTON_NONE;
    s_initialized = true;

    /*
     * The temperature sensor is optional for the UI:
     * if initialization fails, the service screen remains available
     * and displays "--.-C".
     */
    (void)max6675_set_callback(
        service_temperature_handler,
        NULL
    );

    esp_err_t temperature_err =
        max6675_init();

    if (temperature_err == ESP_OK) {
        temperature_err =
            max6675_start_task(500U);
    }

    if (temperature_err != ESP_OK &&
        temperature_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(
            TAG,
            "MAX6675 startup failed: %s",
            esp_err_to_name(temperature_err)
        );
    }

    ESP_LOGI(TAG, "Service screen initialized");
    return ESP_OK;
}

esp_err_t service_screen_show(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    s_visible = true;
    s_pressed_button = SERVICE_BUTTON_NONE;
    draw_screen();

    const esp_err_t err = rgb_display_present();

    xSemaphoreGiveRecursive(s_mutex);
    return err;
}

esp_err_t service_screen_hide(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    s_visible = false;
    s_pressed_button = SERVICE_BUTTON_NONE;
    xSemaphoreGiveRecursive(s_mutex);

    return ESP_OK;
}

bool service_screen_is_visible(void)
{
    return s_visible;
}

void service_screen_set_load_percent(uint8_t percent)
{
    if (!s_initialized) {
        return;
    }

    if (percent > 100) {
        percent = 100;
    }

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    if (s_model.load_percent != percent) {
        s_model.load_percent = percent;

        if (s_visible) {
            draw_screen();
            (void)rgb_display_present();
        }
    }

    xSemaphoreGiveRecursive(s_mutex);
}

void service_screen_set_load_enabled(bool enabled)
{
    if (!s_initialized) {
        return;
    }

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    if (s_model.load_enabled != enabled) {
        s_model.load_enabled = enabled;

        if (s_visible) {
            draw_screen();
            (void)rgb_display_present();
        }
    }

    xSemaphoreGiveRecursive(s_mutex);
}

void service_screen_set_measurements(
    bool valid,
    float voltage_v,
    float current_a,
    float power_w
)
{
    if (!s_initialized) {
        return;
    }

    if (!isfinite(voltage_v) ||
        !isfinite(current_a) ||
        !isfinite(power_w)) {
        valid = false;
    }

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    s_model.measurement_valid = valid;

    if (valid) {
        s_model.voltage_v = voltage_v;
        s_model.current_a = current_a;
        s_model.power_w = power_w;
    } else {
        s_model.voltage_v = 0.0f;
        s_model.current_a = 0.0f;
        s_model.power_w = 0.0f;
    }

    if (s_visible) {
        draw_measurements();
        (void)rgb_display_present();
    }

    xSemaphoreGiveRecursive(s_mutex);
}

void service_screen_set_temperature(
    bool valid,
    float temperature_c
)
{
    if (!s_initialized) {
        return;
    }

    if (!isfinite(temperature_c)) {
        valid = false;
    }

    xSemaphoreTakeRecursive(
        s_mutex,
        portMAX_DELAY
    );

    s_model.temperature_valid = valid;
    s_model.temperature_c =
        valid ? temperature_c : 0.0f;

    if (s_visible) {
        draw_measurements();
        (void)rgb_display_present();
    }

    xSemaphoreGiveRecursive(s_mutex);
}

service_screen_model_t service_screen_get_model(void)
{
    service_screen_model_t model = {0};

    if (!s_initialized) {
        return model;
    }

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    model = s_model;
    xSemaphoreGiveRecursive(s_mutex);

    return model;
}

esp_err_t service_screen_handle_touch(
    uint16_t x,
    uint16_t y,
    bool pressed
)
{
    if (!s_initialized ||
        !s_visible ||
        x >= UI_W ||
        y >= UI_H) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);

    const service_button_t hit = hit_test(x, y);

    if (pressed) {
        if (hit != s_pressed_button) {
            s_pressed_button = hit;
            draw_screen();

            const esp_err_t err =
                rgb_display_present();

            xSemaphoreGiveRecursive(s_mutex);
            return err;
        }

        xSemaphoreGiveRecursive(s_mutex);
        return ESP_OK;
    }

    const service_button_t released =
        s_pressed_button;

    s_pressed_button = SERVICE_BUTTON_NONE;

    if (released != SERVICE_BUTTON_NONE &&
        released == hit) {
        switch (released) {
            case SERVICE_BUTTON_BACK:
                emit(SERVICE_SCREEN_EVENT_BACK);
                break;

            case SERVICE_BUTTON_MINUS:
                if (s_model.load_percent >= 5) {
                    s_model.load_percent -= 5;
                } else {
                    s_model.load_percent = 0;
                }

                ESP_LOGI(
                    TAG,
                    "Stub load set to %u%%",
                    (unsigned)s_model.load_percent
                );

                emit(SERVICE_SCREEN_EVENT_LOAD_CHANGED);
                break;

            case SERVICE_BUTTON_PLUS:
                if (s_model.load_percent <= 95) {
                    s_model.load_percent += 5;
                } else {
                    s_model.load_percent = 100;
                }

                ESP_LOGI(
                    TAG,
                    "Stub load set to %u%%",
                    (unsigned)s_model.load_percent
                );

                emit(SERVICE_SCREEN_EVENT_LOAD_CHANGED);
                break;

            case SERVICE_BUTTON_ENABLE:
                s_model.load_enabled =
                    !s_model.load_enabled;

                ESP_LOGI(
                    TAG,
                    "Stub load %s",
                    s_model.load_enabled
                        ? "enabled"
                        : "disabled"
                );

                emit(
                    SERVICE_SCREEN_EVENT_LOAD_ENABLED_CHANGED
                );
                break;

            case SERVICE_BUTTON_NONE:
            default:
                break;
        }
    }

    draw_screen();
    const esp_err_t err = rgb_display_present();

    xSemaphoreGiveRecursive(s_mutex);
    return err;
}
