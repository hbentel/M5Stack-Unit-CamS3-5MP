idf_component_register(SRCS "ota_mgr.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_update esp_http_client esp_system mqtt_mgr recovery_mgr esp_wifi mbedtls)
