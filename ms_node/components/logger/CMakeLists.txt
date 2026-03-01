idf_component_register(
    SRCS "logger.c" "blockbuf.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES spiffs compression esp_timer
)