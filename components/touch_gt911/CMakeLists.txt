idf_component_register(
    SRCS
        "touch_gt911.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        board
        io_expander
        esp_driver_gpio
        esp_driver_i2c
        freertos
        
)
