/**
 * @file sd_driver.h
 * @brief SD Card Driver public API (ESP32-S3, native SDMMC 1-bit)
 *
 * - Thread-safe (all operations serialized by a mutex)
 * - Power control via SD_EN and card detect via SDC0_DET
 * - File / directory / space / format operations on a FAT filesystem
 * - Writes use fflush + fsync + verify for reliable persistence
 *
 * Paths may be absolute ("/sdcard/img/a.bin") or relative to the mount
 * point ("img/a.bin").
 *
 * NOTE: FatFs long file names are disabled by default in sdkconfig
 * (CONFIG_FATFS_LFN_NONE). Use 8.3 names, or enable
 * CONFIG_FATFS_LFN_HEAP in menuconfig for long names.
 */

#ifndef SD_DRIVER_H
#define SD_DRIVER_H

#include "sd_driver_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_DEFAULT_MOUNT_POINT  "/sdcard"

/* ============================ Init / management ============================ */

/**
 * @brief Power up the card and mount the FAT filesystem
 * @param config Configuration, or NULL for board defaults (main/pindefine.h)
 * @return ESP_OK, ESP_ERR_NO_MEM, or the SDMMC/FATFS error from mounting
 */
esp_err_t sd_driver_init(const sd_driver_config_t *config);

/**
 * @brief Unmount the filesystem and switch the card supply off
 */
esp_err_t sd_driver_deinit(void);

/** @brief true if the filesystem is mounted */
bool sd_driver_is_mounted(void);

/**
 * @brief Read the card-detect switch
 * @return true if a card is in the socket (always true if no CD pin)
 */
bool sd_driver_card_present(void);

/** @brief Get card name, capacity, sector size and type */
esp_err_t sd_driver_get_card_info(sd_driver_card_info_t *info);

/** @brief Board default pins from main/pindefine.h */
sd_driver_pin_config_t sd_driver_get_default_pins(void);

/** @brief Mount point in use (e.g. "/sdcard") */
const char *sd_driver_get_mount_point(void);

/* ============================== Space info ================================= */

esp_err_t sd_driver_get_total_space(uint64_t *total_bytes);
esp_err_t sd_driver_get_free_space(uint64_t *free_bytes);
esp_err_t sd_driver_get_used_space(uint64_t *used_bytes);
esp_err_t sd_driver_get_space_info(sd_driver_space_info_t *space_info);

/* ========================== Directory operations =========================== */

/** @brief Create a directory (ESP_OK if it already exists) */
esp_err_t sd_driver_create_directory(const char *path);

/**
 * @brief List a directory's entries (excluding "." and "..")
 * @note Free the result with sd_driver_free_dir_list()
 */
esp_err_t sd_driver_list_directory(const char *path, sd_driver_dir_list_t *list);

void sd_driver_free_dir_list(sd_driver_dir_list_t *list);

bool sd_driver_directory_exists(const char *path);

/** @brief Total size of all files under a directory (recursive) */
esp_err_t sd_driver_get_directory_size(const char *path, uint64_t *total_size);

/** @brief Delete a directory and everything in it */
esp_err_t sd_driver_delete_directory(const char *path);

/* ============================ File operations ============================== */

/**
 * @brief Write (overwrite) a file, creating the parent directory if needed.
 *        Flushes, fsyncs and verifies the size on disk.
 */
esp_err_t sd_driver_write_file(const char *filepath, const uint8_t *data, size_t size);

/**
 * @brief Append data to a file (created if missing). Flushes and fsyncs.
 */
esp_err_t sd_driver_append_file(const char *filepath, const uint8_t *data, size_t size);

/**
 * @brief Read a file, up to buffer_size bytes
 * @param bytes_read Actual bytes read (may be NULL)
 */
esp_err_t sd_driver_read_file(const char *filepath, uint8_t *buffer, size_t buffer_size, size_t *bytes_read);

/**
 * @brief Read part of a file starting at offset (useful for streaming
 *        large images in chunks without a full-size buffer)
 */
esp_err_t sd_driver_read_file_at(const char *filepath, size_t offset, uint8_t *buffer,
                                 size_t length, size_t *bytes_read);

esp_err_t sd_driver_delete_file(const char *filepath);

bool sd_driver_file_exists(const char *filepath);

esp_err_t sd_driver_get_file_size(const char *filepath, size_t *size);

/* =============================== Format ==================================== */

/**
 * @brief Format the mounted card as FAT. ALL DATA IS LOST.
 */
esp_err_t sd_driver_format(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_DRIVER_H */
