#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_STATE_BOOT = 0,
    APP_STATE_STARTUP_ANIMATION,
    APP_STATE_HOME,
    APP_STATE_HEATING,
    APP_STATE_READY,
    APP_STATE_ACTIVE,
    APP_STATE_COOLDOWN,
    APP_STATE_ERROR,
} app_state_t;

void app_state_set(app_state_t state);
app_state_t app_state_get(void);
const char *app_state_name(app_state_t state);

#ifdef __cplusplus
}
#endif
