/**
 * @file sd_driver.c
 * @brief SD Card Driver implementation (ESP32-S3 native SDMMC, 1-bit mode)
 *
 * Ported from the 7.3" EPD Photoframe SD_card_Driver (SDMMC/Hussan path),
 * adapted to the Fraimic V2.5 13.3" board pins (see main/pindefine.h).
 */

#include "sd_driver.h"
#include "pindefine.h"   /* SD_PIN_* and SD timing/level settings */
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "SD_DRIVER";

/* ============================================================================
 * STATE
 * ============================================================================ */

static sdmmc_card_t *s_card = NULL;
static char s_mount_point[32] = SD_DEFAULT_MOUNT_POINT;
static bool s_is_mounted = false;
static SemaphoreHandle_t s_sd_mutex = NULL;
static sd_driver_pin_config_t s_pins = { -1, -1, -1, -1, -1 };
static uint32_t s_max_files = 5;

/* Timing workarounds carried over from the reference driver */
#define SD_DELAY_AFTER_WRITE_MS     200
#define SD_DELAY_AFTER_MKDIR_MS     100
#define SD_DELAY_BEFORE_UNMOUNT_MS  150

#define SD_READ_CHUNK               4096
#define SD_PATH_MAX                 256

/* ============================================================================
 * LOCKING
 *
 * lock_if_mounted() takes the mutex and returns ESP_OK only if the card is
 * mounted. On any error it does NOT hold the mutex, so callers can simply
 * return its result.
 * ============================================================================ */

#define SD_UNLOCK() xSemaphoreGive(s_sd_mutex)

static esp_err_t ensure_mutex(void)
{
    if (s_sd_mutex == NULL) {
        s_sd_mutex = xSemaphoreCreateMutex();
        if (s_sd_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t lock_if_mounted(void)
{
    if (s_sd_mutex == NULL) {
        ESP_LOGE(TAG, "SD driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    if (!s_is_mounted) {
        SD_UNLOCK();
        ESP_LOGE(TAG, "SD card not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* ============================================================================
 * HELPERS
 * ============================================================================ */

static void build_full_path(const char *path, char *full_path, size_t max_len)
{
    if (path[0] == '/') {
        snprintf(full_path, max_len, "%s", path);
    } else {
        snprintf(full_path, max_len, "%s/%s", s_mount_point, path);
    }
}

/* Create every missing directory in the parent path of filepath */
static esp_err_t ensure_parent_directory(const char *filepath)
{
    char dir[SD_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", filepath);

    char *last_slash = strrchr(dir, '/');
    if (last_slash == NULL || last_slash == dir) {
        return ESP_OK;
    }
    *last_slash = '\0';

    struct stat st;
    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    /* Walk the path, skipping the mount point itself */
    size_t mp_len = strlen(s_mount_point);
    char *p = dir + ((strncmp(dir, s_mount_point, mp_len) == 0) ? mp_len + 1 : 1);
    for (;; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            if (stat(dir, &st) != 0) {
                ESP_LOGI(TAG, "Creating directory: %s", dir);
                if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
                    ESP_LOGE(TAG, "Failed to create directory %s (errno=%d)", dir, errno);
                    return ESP_FAIL;
                }
                vTaskDelay(pdMS_TO_TICKS(SD_DELAY_AFTER_MKDIR_MS));
            }
            *p = saved;
            if (saved == '\0') {
                break;
            }
        }
    }
    return ESP_OK;
}

static void calculate_dir_size_recursive(const char *path, uint64_t *total_size)
{
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        return;
    }

    struct dirent *entry;
    struct stat st;
    char full_path[512];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                calculate_dir_size_recursive(full_path, total_size);
            } else {
                *total_size += st.st_size;
            }
        }
    }
    closedir(dir);
}

static esp_err_t delete_directory_recursive(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory for deletion: %s", path);
        return ESP_FAIL;
    }

    struct dirent *entry;
    struct stat st;
    char full_path[512];
    esp_err_t ret = ESP_OK;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (stat(full_path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            ret = delete_directory_recursive(full_path);
        } else if (unlink(full_path) != 0) {
            ESP_LOGE(TAG, "Failed to delete file: %s", full_path);
            ret = ESP_FAIL;
        }
        if (ret != ESP_OK) {
            break;
        }
    }
    closedir(dir);

    if (ret == ESP_OK && rmdir(path) != 0) {
        ESP_LOGE(TAG, "Failed to remove directory: %s", path);
        ret = ESP_FAIL;
    }
    return ret;
}

/* ============================================================================
 * POWER / CARD DETECT
 * ============================================================================ */

static void sd_power_set(const sd_driver_pin_config_t *pins, bool on)
{
    if (pins->power_en_gpio < 0) {
        return;
    }
    gpio_set_level(pins->power_en_gpio, on ? SD_POWER_ON_LEVEL : !SD_POWER_ON_LEVEL);
}

static void sd_control_pins_init(const sd_driver_pin_config_t *pins)
{
    if (pins->power_en_gpio >= 0) {
        gpio_config_t pwr_conf = {
            .pin_bit_mask = 1ULL << pins->power_en_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_set_level(pins->power_en_gpio, !SD_POWER_ON_LEVEL);
        gpio_config(&pwr_conf);
    }

    if (pins->cd_gpio >= 0) {
        gpio_config_t cd_conf = {
            .pin_bit_mask = 1ULL << pins->cd_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cd_conf);
    }
}

/* Float the bus pins so the ESP doesn't back-power the card through its
 * internal pull-ups while SD_3V3 is off. */
static void sd_bus_pins_release(const sd_driver_pin_config_t *pins)
{
    uint64_t mask = 0;
    if (pins->clk_gpio >= 0) mask |= 1ULL << pins->clk_gpio;
    if (pins->cmd_gpio >= 0) mask |= 1ULL << pins->cmd_gpio;
    if (pins->d0_gpio  >= 0) mask |= 1ULL << pins->d0_gpio;
    if (mask == 0) {
        return;
    }
    gpio_config_t conf = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&conf);
}

bool sd_driver_card_present(void)
{
    int cd = (s_pins.cd_gpio >= 0) ? s_pins.cd_gpio : SD_PIN_CD;
    if (cd < 0) {
        return true;
    }
    if (s_pins.cd_gpio < 0) {
        /* Not initialized yet: make sure the pin is a pulled-up input */
        sd_driver_pin_config_t p = sd_driver_get_default_pins();
        p.power_en_gpio = -1;
        sd_control_pins_init(&p);
    }
    return gpio_get_level(cd) == SD_CD_ACTIVE_LEVEL;
}

/* ============================================================================
 * MOUNT
 * ============================================================================ */

static esp_err_t mount_sd_card(const sd_driver_pin_config_t *pins, const char *mount_point,
                               uint32_t max_files, bool format_if_mount_failed)
{
    ESP_LOGI(TAG, "SDMMC 1-bit: CLK=GPIO%d CMD=GPIO%d D0=GPIO%d PWR=GPIO%d CD=GPIO%d",
             pins->clk_gpio, pins->cmd_gpio, pins->d0_gpio,
             pins->power_en_gpio, pins->cd_gpio);

    sd_control_pins_init(pins);

    if (pins->cd_gpio >= 0) {
        bool present = gpio_get_level(pins->cd_gpio) == SD_CD_ACTIVE_LEVEL;
        ESP_LOGI(TAG, "Card detect GPIO%d = %d (%s)", pins->cd_gpio,
                 gpio_get_level(pins->cd_gpio), present ? "card present" : "no card");
        if (!present) {
            ESP_LOGW(TAG, "Card detect says no card - trying to mount anyway");
        }
    }

    sd_power_set(pins, true);
    vTaskDelay(pdMS_TO_TICKS(SD_POWER_ON_DELAY_MS));

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = max_files,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SD_MAX_FREQ_KHZ;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = pins->clk_gpio;
    slot_config.cmd = pins->cmd_gpio;
    slot_config.d0 = pins->d0_gpio;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.gpio_cd = GPIO_NUM_NC;   /* CD handled in software above */
    slot_config.gpio_wp = GPIO_NUM_NC;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem (card not FAT formatted?)");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        s_card = NULL;
        sd_bus_pins_release(pins);
        sd_power_set(pins, false);
        return ret;
    }
    return ESP_OK;
}

/* ============================================================================
 * INIT / DEINIT
 * ============================================================================ */

sd_driver_pin_config_t sd_driver_get_default_pins(void)
{
    sd_driver_pin_config_t pins = {
        .clk_gpio = SD_PIN_CLK,
        .cmd_gpio = SD_PIN_CMD,
        .d0_gpio = SD_PIN_D0,
        .power_en_gpio = SD_PIN_POWER_EN,
        .cd_gpio = SD_PIN_CD,
    };
    return pins;
}

esp_err_t sd_driver_init(const sd_driver_config_t *config)
{
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_sd_mutex, portMAX_DELAY);

    if (s_is_mounted) {
        ESP_LOGW(TAG, "SD card already mounted");
        SD_UNLOCK();
        return ESP_OK;
    }

    const char *mount_point = SD_DEFAULT_MOUNT_POINT;
    bool format_if_failed = false;
    s_max_files = 5;

    if (config != NULL) {
        s_pins = config->pin_config;
        if (config->mount_point != NULL) {
            mount_point = config->mount_point;
        }
        if (config->max_files > 0) {
            s_max_files = config->max_files;
        }
        format_if_failed = config->format_if_mount_failed;
    } else {
        s_pins = sd_driver_get_default_pins();
    }

    snprintf(s_mount_point, sizeof(s_mount_point), "%s", mount_point);

    ESP_LOGI(TAG, "Initializing SD card");
    ret = mount_sd_card(&s_pins, s_mount_point, s_max_files, format_if_failed);
    if (ret == ESP_OK) {
        s_is_mounted = true;
        ESP_LOGI(TAG, "SD card mounted at %s", s_mount_point);
        sdmmc_card_print_info(stdout, s_card);
    }

    SD_UNLOCK();
    return ret;
}

esp_err_t sd_driver_deinit(void)
{
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return (ret == ESP_ERR_INVALID_STATE) ? ESP_OK : ret;
    }

    ESP_LOGI(TAG, "Unmounting SD card");
    vTaskDelay(pdMS_TO_TICKS(SD_DELAY_BEFORE_UNMOUNT_MS));

    ret = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
        SD_UNLOCK();
        return ret;
    }

    s_is_mounted = false;
    s_card = NULL;

    sd_bus_pins_release(&s_pins);
    sd_power_set(&s_pins, false);

    ESP_LOGI(TAG, "SD card unmounted and powered off");
    SD_UNLOCK();
    return ESP_OK;
}

bool sd_driver_is_mounted(void)
{
    return s_is_mounted;
}

const char *sd_driver_get_mount_point(void)
{
    return s_mount_point;
}

esp_err_t sd_driver_get_card_info(sd_driver_card_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(info->name, sizeof(info->name), "%s", s_card->cid.name);
    info->capacity_mb = ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024);
    info->sector_size = s_card->csd.sector_size;

    if (info->capacity_mb <= 2048) {
        snprintf(info->type, sizeof(info->type), "SDSC");
    } else if (info->capacity_mb <= 32768) {
        snprintf(info->type, sizeof(info->type), "SDHC");
    } else {
        snprintf(info->type, sizeof(info->type), "SDXC");
    }

    SD_UNLOCK();
    return ESP_OK;
}

/* ============================================================================
 * SPACE INFO
 * ============================================================================ */

esp_err_t sd_driver_get_space_info(sd_driver_space_info_t *space_info)
{
    if (space_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    uint64_t total = 0, free_b = 0;
    ret = esp_vfs_fat_info(s_mount_point, &total, &free_b);
    if (ret == ESP_OK) {
        space_info->total_bytes = total;
        space_info->free_bytes = free_b;
        space_info->used_bytes = total - free_b;
    } else {
        ESP_LOGE(TAG, "Failed to get filesystem info: %s", esp_err_to_name(ret));
    }

    SD_UNLOCK();
    return ret;
}

esp_err_t sd_driver_get_total_space(uint64_t *total_bytes)
{
    if (total_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sd_driver_space_info_t info;
    esp_err_t ret = sd_driver_get_space_info(&info);
    if (ret == ESP_OK) {
        *total_bytes = info.total_bytes;
    }
    return ret;
}

esp_err_t sd_driver_get_free_space(uint64_t *free_bytes)
{
    if (free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sd_driver_space_info_t info;
    esp_err_t ret = sd_driver_get_space_info(&info);
    if (ret == ESP_OK) {
        *free_bytes = info.free_bytes;
    }
    return ret;
}

esp_err_t sd_driver_get_used_space(uint64_t *used_bytes)
{
    if (used_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sd_driver_space_info_t info;
    esp_err_t ret = sd_driver_get_space_info(&info);
    if (ret == ESP_OK) {
        *used_bytes = info.used_bytes;
    }
    return ret;
}

/* ============================================================================
 * DIRECTORY OPERATIONS
 * ============================================================================ */

esp_err_t sd_driver_create_directory(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    char full_path[SD_PATH_MAX];
    build_full_path(path, full_path, sizeof(full_path));

    struct stat st;
    if (stat(full_path, &st) == 0) {
        ret = S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Path exists but is not a directory: %s", full_path);
        }
        SD_UNLOCK();
        return ret;
    }

    /* ensure_parent_directory() creates all components of the parent path;
     * append a dummy leaf so it creates full_path itself too. */
    char leaf_path[SD_PATH_MAX + 2];
    snprintf(leaf_path, sizeof(leaf_path), "%s/x", full_path);
    ret = ensure_parent_directory(leaf_path);

    if (ret == ESP_OK && (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode))) {
        ESP_LOGE(TAG, "Directory creation verification failed: %s", full_path);
        ret = ESP_FAIL;
    }

    SD_UNLOCK();
    return ret;
}

esp_err_t sd_driver_list_directory(const char *path, sd_driver_dir_list_t *list)
{
    if (path == NULL || list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    list->entries = NULL;
    list->count = 0;

    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    char full_path[SD_PATH_MAX];
    build_full_path(path, full_path, sizeof(full_path));

    DIR *dir = opendir(full_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", full_path);
        SD_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            count++;
        }
    }

    if (count == 0) {
        closedir(dir);
        SD_UNLOCK();
        return ESP_OK;
    }

    list->entries = (sd_driver_dir_entry_t *)malloc(count * sizeof(sd_driver_dir_entry_t));
    if (list->entries == NULL) {
        ESP_LOGE(TAG, "Out of memory for directory listing (%" PRIu32 " entries)", count);
        closedir(dir);
        SD_UNLOCK();
        return ESP_ERR_NO_MEM;
    }

    rewinddir(dir);
    uint32_t index = 0;
    char entry_path[512];
    struct stat st;
    while (index < count && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        sd_driver_dir_entry_t *e = &list->entries[index];
        snprintf(e->name, sizeof(e->name), "%s", entry->d_name);

        snprintf(entry_path, sizeof(entry_path), "%s/%s", full_path, entry->d_name);
        if (stat(entry_path, &st) == 0) {
            e->type = S_ISDIR(st.st_mode) ? SD_ENTRY_TYPE_DIRECTORY : SD_ENTRY_TYPE_FILE;
        } else {
            e->type = SD_ENTRY_TYPE_UNKNOWN;
        }
        index++;
    }
    list->count = index;
    closedir(dir);

    ESP_LOGI(TAG, "Listed %" PRIu32 " entries in %s", list->count, full_path);
    SD_UNLOCK();
    return ESP_OK;
}

void sd_driver_free_dir_list(sd_driver_dir_list_t *list)
{
    if (list != NULL) {
        free(list->entries);
        list->entries = NULL;
        list->count = 0;
    }
}

bool sd_driver_directory_exists(const char *path)
{
    if (path == NULL || lock_if_mounted() != ESP_OK) {
        return false;
    }
    char full_path[SD_PATH_MAX];
    build_full_path(path, full_path, sizeof(full_path));

    struct stat st;
    bool exists = (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));
    SD_UNLOCK();
    return exists;
}

esp_err_t sd_driver_get_directory_size(const char *path, uint64_t *total_size)
{
    if (path == NULL || total_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    char full_path[SD_PATH_MAX];
    build_full_path(path, full_path, sizeof(full_path));

    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGE(TAG, "Directory not found: %s", full_path);
        SD_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }

    *total_size = 0;
    calculate_dir_size_recursive(full_path, total_size);
    ESP_LOGI(TAG, "Directory %s total size: %llu bytes", full_path, (unsigned long long)*total_size);

    SD_UNLOCK();
    return ESP_OK;
}

esp_err_t sd_driver_delete_directory(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    char full_path[SD_PATH_MAX];
    build_full_path(path, full_path, sizeof(full_path));

    if (strcmp(full_path, s_mount_point) == 0) {
        ESP_LOGE(TAG, "Refusing to delete the mount point; use sd_driver_format()");
        SD_UNLOCK();
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(full_path, &st) != 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (!S_ISDIR(st.st_mode)) {
        ESP_LOGE(TAG, "Path is not a directory: %s", full_path);
        ret = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "Deleting directory recursively: %s", full_path);
        ret = delete_directory_recursive(full_path);
    }

    SD_UNLOCK();
    return ret;
}

/* ============================================================================
 * FILE OPERATIONS
 * ============================================================================ */

static esp_err_t write_common(const char *filepath, const uint8_t *data, size_t size, bool append)
{
    if (filepath == NULL || data == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    char full_path[SD_PATH_MAX];
    build_full_path(filepath, full_path, sizeof(full_path));

    ret = ensure_parent_directory(full_path);
    if (ret != ESP_OK) {
        SD_UNLOCK();
        return ret;
    }

    size_t prev_size = 0;
    struct stat st;
    if (append && stat(full_path, &st) == 0) {
        prev_size = st.st_size;
    }

    ESP_LOGI(TAG, "%s file: %s (%u bytes)", append ? "Appending" : "Writing",
             full_path, (unsigned)size);

    FILE *f = fopen(full_path, append ? "ab" : "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s (errno=%d)", full_path, errno);
        SD_UNLOCK();
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, size, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (written != size) {
        ESP_LOGE(TAG, "Write size mismatch: wrote %u, expected %u (card full?)",
                 (unsigned)written, (unsigned)size);
        SD_UNLOCK();
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(SD_DELAY_AFTER_WRITE_MS));

    if (stat(full_path, &st) != 0 || (size_t)st.st_size != prev_size + size) {
        ESP_LOGE(TAG, "File verification failed: %s", full_path);
        SD_UNLOCK();
        return ESP_FAIL;
    }

    SD_UNLOCK();
    return ESP_OK;
}

esp_err_t sd_driver_write_file(const char *filepath, const uint8_t *data, size_t size)
{
    return write_common(filepath, data, size, false);
}

esp_err_t sd_driver_append_file(const char *filepath, const uint8_t *data, size_t size)
{
    return write_common(filepath, data, size, true);
}

esp_err_t sd_driver_read_file_at(const char *filepath, size_t offset, uint8_t *buffer,
                                 size_t length, size_t *bytes_read)
{
    if (bytes_read != NULL) {
        *bytes_read = 0;
    }
    if (filepath == NULL || buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    char full_path[SD_PATH_MAX];
    build_full_path(filepath, full_path, sizeof(full_path));

    FILE *f = fopen(full_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", full_path);
        SD_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }

    if (offset > 0 && fseek(f, (long)offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Seek to %u failed: %s", (unsigned)offset, full_path);
        fclose(f);
        SD_UNLOCK();
        return ESP_FAIL;
    }

    size_t total_read = 0;
    while (total_read < length) {
        size_t remaining = length - total_read;
        size_t chunk = remaining < SD_READ_CHUNK ? remaining : SD_READ_CHUNK;
        size_t n = fread(buffer + total_read, 1, chunk, f);
        if (n == 0) {
            break;
        }
        total_read += n;

        /* Yield periodically during long reads (~every 64 KB) */
        if ((total_read & 0xFFFF) == 0) {
            vTaskDelay(1);
        }
    }

    bool io_error = ferror(f);
    fclose(f);

    if (bytes_read != NULL) {
        *bytes_read = total_read;
    }

    SD_UNLOCK();

    if (io_error) {
        ESP_LOGE(TAG, "Read error: %s", full_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sd_driver_read_file(const char *filepath, uint8_t *buffer, size_t buffer_size, size_t *bytes_read)
{
    esp_err_t ret = sd_driver_read_file_at(filepath, 0, buffer, buffer_size, bytes_read);
    if (ret == ESP_OK && bytes_read != NULL) {
        ESP_LOGI(TAG, "Read %u bytes from %s", (unsigned)*bytes_read, filepath);
    }
    return ret;
}

esp_err_t sd_driver_delete_file(const char *filepath)
{
    if (filepath == NULL || filepath[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    char full_path[SD_PATH_MAX];
    build_full_path(filepath, full_path, sizeof(full_path));

    struct stat st;
    if (stat(full_path, &st) != 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (S_ISDIR(st.st_mode)) {
        ESP_LOGE(TAG, "Path is a directory, not a file: %s", full_path);
        ret = ESP_FAIL;
    } else if (unlink(full_path) != 0 || stat(full_path, &st) == 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", full_path);
        ret = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "Deleted file: %s", full_path);
    }

    SD_UNLOCK();
    return ret;
}

bool sd_driver_file_exists(const char *filepath)
{
    if (filepath == NULL || lock_if_mounted() != ESP_OK) {
        return false;
    }
    char full_path[SD_PATH_MAX];
    build_full_path(filepath, full_path, sizeof(full_path));

    struct stat st;
    bool exists = (stat(full_path, &st) == 0 && S_ISREG(st.st_mode));
    SD_UNLOCK();
    return exists;
}

esp_err_t sd_driver_get_file_size(const char *filepath, size_t *size)
{
    if (filepath == NULL || size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    char full_path[SD_PATH_MAX];
    build_full_path(filepath, full_path, sizeof(full_path));

    struct stat st;
    if (stat(full_path, &st) != 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (!S_ISREG(st.st_mode)) {
        ret = ESP_FAIL;
    } else {
        *size = st.st_size;
    }

    SD_UNLOCK();
    return ret;
}

/* ============================================================================
 * FORMAT
 * ============================================================================ */

esp_err_t sd_driver_format(void)
{
    esp_err_t ret = lock_if_mounted();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGW(TAG, "Formatting SD card - ALL DATA WILL BE LOST!");
    ret = esp_vfs_fat_sdcard_format(s_mount_point, s_card);
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "SD card formatted successfully");
    } else {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(ret));
    }

    SD_UNLOCK();
    return ret;
}
