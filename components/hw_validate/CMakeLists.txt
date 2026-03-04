idf_component_register(
    SRCS "hw_validate.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio esp_driver_ledc esp_psram
)
