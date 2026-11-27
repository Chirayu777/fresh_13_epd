//=================================================================================================
//                   Pin Define for 13.3" Control Board
//
// File Name : pindefine.h
// Author : Electronic Design Dept. II
// Data : 2024.04.29
// Version : 1.0
// Copyright : E Ink Holdings Inc.
//=================================================================================================

#ifndef __PINDEFINE_H__
#define __PINDEFINE_H__

//==============  Standard SPI Setting   ==============//
//Please modify the pin number
#define SPI_CS0		40
#define SPI_CS1		39
#define SPI_CLK		46
#define SPI_Data0	41	// 3-wire SPI: bidirectional SDA (TX + RX)
#define SPI_Data1	11	// Unused in 3-wire SPI mode
#define SPI_Data2	39
#define SPI_Data3	38

//==============   GPIO Setting   ==============//
//Please modify the pin number
#define EPD_BUSY	7   // Please set it as input pin
#define EPD_RST		6   // Please set it as output pin
#define LOAD_SW		17 // Please set it as output pin

//==============   SD Card (SDMMC 1-bit) Setting   ==============//
// Schematic SCH_Fraimic_V2.5: TF CARD / TF CARD Power Control
#define SD_PIN_POWER_EN		45	// SD_EN    : HIGH = SD_3V3 on (R28 pulls low at boot)
#define SD_PIN_CD			48	// SDC0_DET : card detect input
#define SD_PIN_CMD			47	// SDC0_CMD
#define SD_PIN_D0			21	// SDC0_D0
#define SD_PIN_CLK			14	// SDC0_CLK

#define SD_POWER_ON_LEVEL	1		// SD_EN level that powers the card
#define SD_CD_ACTIVE_LEVEL	0		// SDC0_DET level when card inserted (flip to 1 if reversed)
#define SD_POWER_ON_DELAY_MS	50	// SD_3V3 ramp-up time
#define SD_MAX_FREQ_KHZ		20000	// SDMMC clock; drop to 4000 on CRC/timeout errors

//===============================================

#define GPIO_LOW	0
#define GPIO_HIGH	1

#endif //#ifndef __PINDEFINE_H__
