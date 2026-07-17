idf_component_register(
    SRCS "rgb_display.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        esp_lcd
        freertos
)
