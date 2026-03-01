idf_component_register(SRCS "storage_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES spiffs compression esp_timer esp_common heap)
