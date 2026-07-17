#include "app_state.h"

#include <stdatomic.h>

static atomic_int s_state = APP_STATE_BOOT;

void app_state_set(app_state_t state)
{
    atomic_store(&s_state, (int)state);
}

app_state_t app_state_get(void)
{
    return (app_state_t)atomic_load(&s_state);
}

const char *app_state_name(app_state_t state)
{
    switch (state) {
        case APP_STATE_BOOT: return "BOOT";
        case APP_STATE_STARTUP_ANIMATION: return "STARTUP_ANIMATION";
        case APP_STATE_HOME: return "HOME";
        case APP_STATE_HEATING: return "HEATING";
        case APP_STATE_READY: return "READY";
        case APP_STATE_ACTIVE: return "ACTIVE";
        case APP_STATE_COOLDOWN: return "COOLDOWN";
        case APP_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
