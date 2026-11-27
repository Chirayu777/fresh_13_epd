/**
 * @file sd_driver_types.h
 * @brief SD Card Driver type definitions
 */

#ifndef SD_DRIVER_TYPES_H
#define SD_DRIVER_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SD card pin configuration (native SDMMC, 1-bit mode)
 */
typedef struct {
    int clk_gpio;       /**< SDMMC CLK */
    int cmd_gpio;       /**< SDMMC CMD */
    int d0_gpio;        /**< SDMMC DAT0 */
    int power_en_gpio;  /**< SD supply enable, -1 if unused */
    int cd_gpio;        /**< Card detect input, -1 if unused */
} sd_driver_pin_config_t;

/**
 * @brief SD card information
 */
typedef struct {
    char name[64];          /**< Card name (from CID) */
    uint64_t capacity_mb;   /**< Total capacity in MB */
    uint32_t sector_size;   /**< Sector size in bytes */
    char type[32];          /**< SDSC / SDHC / SDXC */
} sd_driver_card_info_t;

/**
 * @brief Storage space information
 */
typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
} sd_driver_space_info_t;

/**
 * @brief Directory entry type
 */
typedef enum {
    SD_ENTRY_TYPE_FILE,
    SD_ENTRY_TYPE_DIRECTORY,
    SD_ENTRY_TYPE_UNKNOWN
} sd_driver_entry_type_t;

/**
 * @brief Directory entry
 */
typedef struct {
    char name[256];
    sd_driver_entry_type_t type;
} sd_driver_dir_entry_t;

/**
 * @brief Directory listing (free with sd_driver_free_dir_list())
 */
typedef struct {
    sd_driver_dir_entry_t *entries;
    uint32_t count;
} sd_driver_dir_list_t;

/**
 * @brief SD card driver configuration
 */
typedef struct {
    sd_driver_pin_config_t pin_config;  /**< Pin configuration */
    const char *mount_point;            /**< Mount point (e.g. "/sdcard") */
    uint32_t max_files;                 /**< Max simultaneously open files */
    bool format_if_mount_failed;        /**< Format card if mount fails */
} sd_driver_config_t;

#ifdef __cplusplus
}
#endif

#endif /* SD_DRIVER_TYPES_H */
