#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "pindefine.h"
#include "comm.h"
#include "EL133UF1.h"
#include "status.h"
#include "sd_driver.h"

static void sdCardInit(void)
{
	esp_err_t ret = sd_driver_init(NULL);	// default pins from pindefine.h
	if (ret != ESP_OK) {
		printf("SD card init failed: %s (card present: %d)\r\n",
		       esp_err_to_name(ret), sd_driver_card_present());
		return;
	}

	sd_driver_card_info_t info;
	if (sd_driver_get_card_info(&info) == ESP_OK) {
		printf("SD card: %s, %s, %llu MB\r\n", info.name, info.type,
		       (unsigned long long)info.capacity_mb);
	}

	sd_driver_space_info_t space;
	if (sd_driver_get_space_info(&space) == ESP_OK) {
		printf("SD space: total %llu KB, free %llu KB\r\n",
		       (unsigned long long)(space.total_bytes / 1024),
		       (unsigned long long)(space.free_bytes / 1024));
	}

	// Write/read-back self test
	const char *msg = "EPD 13.3 SD test";
	uint8_t buf[32] = {0};
	size_t n = 0;
	if (sd_driver_write_file("TEST.TXT", (const uint8_t *)msg, strlen(msg)) == ESP_OK &&
	    sd_driver_read_file("TEST.TXT", buf, sizeof(buf) - 1, &n) == ESP_OK) {
		printf("SD self test %s: \"%s\"\r\n",
		       (n == strlen(msg) && memcmp(buf, msg, n) == 0) ? "PASS" : "FAIL", buf);
	}
}

//=============   SD Card Image Display  ==============
// Image .bin format (same as 7.3" Photoframe project, 13.3" panel):
//   1200 x 1600 px, 4 bits/pixel -> 600 bytes per row, 1600 rows = 960000 bytes.
//   In each row, bytes [0..299] go to driver IC CS0 (left half) and
//   bytes [300..599] go to driver IC CS1 (right half).
#define SD_IMAGE_DIR			"/sdcard/IMAGES"
#define SD_IMAGE_ROW_BYTES		600
#define SD_IMAGE_HALF_ROW_BYTES	300
#define SD_IMAGE_ROWS			1600
#define SD_IMAGE_SIZE			(SD_IMAGE_ROW_BYTES * SD_IMAGE_ROWS)
#define SD_IMAGE_CHUNK_ROWS		40		// 40 rows = 24000 bytes per SD read (< SPI max 32768)

// Find the first .bin file (alphabetical) in SD_IMAGE_DIR.
static esp_err_t sdFindFirstImage(char *path, size_t pathLen)
{
	sd_driver_dir_list_t list;
	esp_err_t ret = sd_driver_list_directory(SD_IMAGE_DIR, &list);
	if (ret != ESP_OK) {
		printf("Cannot open %s: %s\r\n", SD_IMAGE_DIR, esp_err_to_name(ret));
		return ret;
	}

	const char *best = NULL;
	for (uint32_t i = 0; i < list.count; i++) {
		const char *name = list.entries[i].name;
		size_t len = strlen(name);
		if (list.entries[i].type != SD_ENTRY_TYPE_FILE || len <= 4 ||
		    strcasecmp(name + len - 4, ".bin") != 0) {
			continue;
		}
		if (best == NULL || strcasecmp(name, best) < 0) {
			best = name;
		}
	}

	if (best == NULL) {
		printf("No .bin images found in %s\r\n", SD_IMAGE_DIR);
		ret = ESP_ERR_NOT_FOUND;
	} else {
		snprintf(path, pathLen, "%s/%s", SD_IMAGE_DIR, best);
		ret = ESP_OK;
	}
	sd_driver_free_dir_list(&list);
	return ret;
}

// Stream one half (csIndex 0 = left, 1 = right) of the image file to its driver IC.
static esp_err_t sdSendImageHalf(const char *path, unsigned char csIndex, uint8_t *buf)
{
	esp_err_t ret = ESP_OK;
	const size_t srcOffset = csIndex * SD_IMAGE_HALF_ROW_BYTES;

	setPinCsAll(GPIO_HIGH);
	setPinCs(csIndex, GPIO_LOW);
	writeEpdCommand(DTM);

	for (int row = 0; row < SD_IMAGE_ROWS; row += SD_IMAGE_CHUNK_ROWS) {
		int rows = SD_IMAGE_ROWS - row;
		if (rows > SD_IMAGE_CHUNK_ROWS) rows = SD_IMAGE_CHUNK_ROWS;

		size_t want = (size_t)rows * SD_IMAGE_ROW_BYTES;
		size_t got = 0;
		ret = sd_driver_read_file_at(path, (size_t)row * SD_IMAGE_ROW_BYTES, buf, want, &got);
		if (ret != ESP_OK || got != want) {
			printf("SD image read failed at row %d (%u/%u bytes)\r\n", row, (unsigned)got, (unsigned)want);
			ret = (ret != ESP_OK) ? ret : ESP_FAIL;
			break;
		}

		// Pack this IC's half-rows together (dest never overtakes src, so in-place is safe)
		for (int r = 0; r < rows; r++) {
			memmove(buf + r * SD_IMAGE_HALF_ROW_BYTES,
			        buf + r * SD_IMAGE_ROW_BYTES + srcOffset,
			        SD_IMAGE_HALF_ROW_BYTES);
		}
		writeEpdData(buf, rows * SD_IMAGE_HALF_ROW_BYTES);
	}

	setPinCsAll(GPIO_HIGH);
	return ret;
}

// Display a 960000-byte .bin image from the SD card.
static esp_err_t epdDisplaySdImage(const char *path)
{
	size_t size = 0;
	esp_err_t ret = sd_driver_get_file_size(path, &size);
	if (ret != ESP_OK) {
		printf("Image not found: %s\r\n", path);
		return ret;
	}
	if (size != SD_IMAGE_SIZE) {
		printf("Image %s is %u bytes, expected %u\r\n", path, (unsigned)size, (unsigned)SD_IMAGE_SIZE);
		return ESP_ERR_INVALID_SIZE;
	}

	uint8_t *buf = heap_caps_malloc(SD_IMAGE_CHUNK_ROWS * SD_IMAGE_ROW_BYTES, MALLOC_CAP_DMA);
	if (buf == NULL) {
		printf("No memory for SD image buffer\r\n");
		return ESP_ERR_NO_MEM;
	}

	printf("Displaying SD image: %s\r\n", path);
	ret = sdSendImageHalf(path, 0, buf);			// CS0: left half
	if (ret == ESP_OK) {
		ret = sdSendImageHalf(path, 1, buf);		// CS1: right half
	}
	free(buf);

	if (ret == ESP_OK) {
		epdDisplay();								// refresh panel
		printf("SD image displayed.\r\n");
	}
	return ret;
}
//====================================================

void app_main(void)
{
//=============   SD Card Initial ====================
	sdCardInit();
//====================================================

//=============   EPD Initial ========================
	initialGpio();
	initialSpi();
	setGpioLevel(LOAD_SW, GPIO_HIGH);	// power on the EPD panel
	delayms(100);
	epdHardwareReset();
	setPinCsAll(GPIO_HIGH);
	initEPD();
	readICTemperature();
//====================================================

//=============   Update EPD  ========================
	//Read image data to epdImageDataBuffer.
	//readFlashMemory(CS0_Address, epdImageDataBuffer, EPD_IMAGE_DATA_BUFFER);
	//Write CS0 image date 480000 bytes
	//writeEpdImage(SPI_CS0, epdImageDataBuffer, EPD_IMAGE_DATA_BUFFER);
	//Read image data to epdImageDataBuffer.
	//readFlashMemory(CS1_Address, epdImageDataBuffer, EPD_IMAGE_DATA_BUFFER);
	//Write CS1 image date 480000 bytes
//	writeEpdImage(SPI_CS1, epdImageDataBuffer, EPD_IMAGE_DATA_BUFFER);
	epdDisplayColorBar();
	epdDisplay();
//====================================================

//=============   Update EPD from SD card  ===========
	if (sd_driver_is_mounted()) {
		char imagePath[300];
		if (sdFindFirstImage(imagePath, sizeof(imagePath)) == ESP_OK) {
			epdDisplaySdImage(imagePath);
		}
	}
//====================================================

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

}