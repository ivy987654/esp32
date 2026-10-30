idf_component_register(SRCS "main.c" "board.c" "ui.c" "link.c" "lv_mem_psram.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd esp_driver_i2c esp_driver_uart esp_driver_gpio esp_timer esp_partition spi_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
