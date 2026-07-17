idf_component_register(
    SRCS
        "tca9554.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        board
        esp_driver_i2c
)
