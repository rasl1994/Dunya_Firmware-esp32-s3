idf_component_register(
    SRCS
        "app_controller.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_state
        board
        heater_control
        ina226
        service_screen
        touch_gt911
        ui_home_dynamic
        freertos
)
