idf_component_register(
    SRCS "uav_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_http_client mbedtls cjson esp_event log storage_manager
)
