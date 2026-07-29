idf_component_register(
    SRCS
        "app_main.c"

    INCLUDE_DIRS
        "."

    PRIV_REQUIRES
        board
        app_state
        display_power
        st7701
        rgb_display
        intro_animation
        nvs_flash
        esp_psram

        touch_gt911
        ui_home_dynamic
		service_screen
        ina226
        heater_control
        max6675

        app_controller

)

# spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)