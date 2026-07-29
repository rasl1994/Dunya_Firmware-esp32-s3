idf_component_register(
    SRCS
        "heater_control.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        io_expander
        esp_driver_ledc
        freertos
)
