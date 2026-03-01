idf_component_register(
    SRCS "rf_receiver.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_rmt freertos log
)
