idf_component_register(
    SRCS "intro_animation.c"
    INCLUDE_DIRS "include"
    REQUIRES
        rgb_display
        spiffs
        esp_timer
)
