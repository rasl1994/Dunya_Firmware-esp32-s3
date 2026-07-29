idf_component_register(
    SRCS
        "service_screen.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        rgb_display
        max6675
        freertos
)
