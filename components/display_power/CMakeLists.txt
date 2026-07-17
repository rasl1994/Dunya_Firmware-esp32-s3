idf_component_register(
    SRCS
        "display_power.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        board
        io_expander
        esp_driver_ledc
)
