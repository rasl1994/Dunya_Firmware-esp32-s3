idf_component_register(
    SRCS
        "st7701.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        board
        esp_driver_spi
        freertos
)
