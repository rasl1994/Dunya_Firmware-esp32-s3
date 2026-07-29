idf_component_register(
    SRCS
        "ina226.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        io_expander
        esp_driver_i2c
        freertos
)
