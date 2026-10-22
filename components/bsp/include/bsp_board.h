#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

#define BSP_POWER_EN       GPIO_NUM_48
#define BSP_BATTERY_ADC    GPIO_NUM_17
#define BSP_BATTERY_R_TOP_OHM    33000U
#define BSP_BATTERY_R_BOTTOM_OHM 10000U
// Board measurement on 2026-10-19, with the current divider and ADC attenuation.
#define BSP_BATTERY_CAL_RAW     2752
#define BSP_BATTERY_CAL_MV      3832U
#define BSP_LCD_LED        GPIO_NUM_15
#define BSP_LCD_RST        GPIO_NUM_14
#define BSP_LCD_DC         GPIO_NUM_10
#define BSP_LCD_MOSI       GPIO_NUM_11
#define BSP_LCD_SCLK       GPIO_NUM_12
#define BSP_LCD_CS         GPIO_NUM_13
#define BSP_LCD_SPI_HOST   SPI2_HOST
#define BSP_LCD_SPI_HZ     (30 * 1000 * 1000) // Requested clock; actual clock follows SPI divider.
#define BSP_LCD_WIDTH      160
#define BSP_LCD_HEIGHT     80
// Vendor demo direction 3: 90 degrees, MADCTL = MY | MV | BGR.
#define BSP_LCD_MADCTL     0xA8
#define BSP_LCD_X_OFFSET   1
#define BSP_LCD_Y_OFFSET   26
#define BSP_LCD_BUFFER_LINES 20
#define BSP_BACKLIGHT_HZ   30000
#define BSP_POWER_SETTLE_MS 20

#define BSP_RADIO_GDO0     GPIO_NUM_9
#define BSP_RADIO_CS       GPIO_NUM_8
#define BSP_RADIO_SCLK     GPIO_NUM_7
#define BSP_RADIO_MOSI     GPIO_NUM_6
#define BSP_RADIO_MISO     GPIO_NUM_5
// The schematic's SDO2 label maps to the module's GDO2 pin.
#define BSP_RADIO_GDO2     GPIO_NUM_4
#define BSP_RADIO_SPI_HOST SPI3_HOST
#define BSP_RADIO_SPI_HZ   (1 * 1000 * 1000)
#define BSP_RADIO_XTAL_HZ  26000000ULL // Default assumption; confirm actual A07 crystal.

#define BSP_BUTTON_UP      GPIO_NUM_1
#define BSP_BUTTON_RIGHT   GPIO_NUM_2
#define BSP_BUTTON_LEFT    GPIO_NUM_18
#define BSP_BUTTON_PRESS   GPIO_NUM_16
#define BSP_BUTTON_DOWN    GPIO_NUM_3
#define BSP_BUTTON_DEBOUNCE_MS 2
