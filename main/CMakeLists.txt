idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES ili9341 framebuffer wifi_server audio)
