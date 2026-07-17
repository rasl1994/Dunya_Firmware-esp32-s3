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
        ui_home_dynamic
        touch_gt911
        nvs_flash
        esp_psram
)

# spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)