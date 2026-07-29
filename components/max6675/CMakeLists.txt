idf_component_register(
    SRCS
        "MAX6675.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        io_expander
        esp_driver_gpio
        freertos
)
