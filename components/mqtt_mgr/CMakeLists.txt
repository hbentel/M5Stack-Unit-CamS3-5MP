if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(JSON_COMPONENT "cjson")   # espressif/cjson managed component
else()
    set(JSON_COMPONENT "json")    # built-in json component (IDF 5.x)
endif()

idf_component_register(SRCS "mqtt_mgr.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqtt esp_wifi ${JSON_COMPONENT} esp_timer esp_https_ota esp32-camera jpeg_validate recovery_mgr heap http_server)
