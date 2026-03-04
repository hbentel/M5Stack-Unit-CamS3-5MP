idf_component_register(SRCS "recovery_mgr.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_timer nvs_flash esp_system app_update)
