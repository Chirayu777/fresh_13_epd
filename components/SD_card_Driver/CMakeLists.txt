# SD card driver (native SDMMC, 1-bit mode) for the 13.3" Fraimic V2.5 board.
# Pin assignments come from main/pindefine.h.

idf_component_register(
    SRCS "sd_driver.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/main"
    REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio freertos
)
