#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_master.h"

namespace board {

// Waveshare ESP32-S3-ePaper-1.54 V2.
// Keep every board-specific assumption in this file.

// ePaper: 200x200, SSD1681-family controller.
constexpr gpio_num_t EPD_PWR  = GPIO_NUM_6; // active LOW: 0 = on, 1 = off
constexpr gpio_num_t EPD_BUSY = GPIO_NUM_8;
constexpr gpio_num_t EPD_RST  = GPIO_NUM_9;
constexpr gpio_num_t EPD_DC   = GPIO_NUM_10;
constexpr gpio_num_t EPD_CS   = GPIO_NUM_11;
constexpr gpio_num_t EPD_SCLK = GPIO_NUM_12;
constexpr gpio_num_t EPD_MOSI = GPIO_NUM_13;
constexpr spi_host_device_t EPD_SPI_HOST = SPI2_HOST;
constexpr int EPD_WIDTH = 200;
constexpr int EPD_HEIGHT = 200;

// Shared I2C bus: ES8311 / PCF85063 / SHTC3.
constexpr i2c_port_num_t I2C_PORT = I2C_NUM_0;
constexpr gpio_num_t I2C_SDA = GPIO_NUM_47;
constexpr gpio_num_t I2C_SCL = GPIO_NUM_48;
constexpr uint8_t ES8311_ADDR = 0x18;

// ES8311 audio interface.
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr gpio_num_t I2S_MCLK = GPIO_NUM_14;
constexpr gpio_num_t I2S_BCLK = GPIO_NUM_15;
constexpr gpio_num_t I2S_DIN  = GPIO_NUM_16; // ES8311 ADC -> ESP32
constexpr gpio_num_t I2S_LRCK = GPIO_NUM_38;
constexpr gpio_num_t I2S_DOUT = GPIO_NUM_45; // ESP32 -> ES8311 DAC
constexpr gpio_num_t AUDIO_PWR = GPIO_NUM_42; // active LOW: 0 = on, 1 = off
constexpr gpio_num_t PA_CTRL   = GPIO_NUM_46;

// TF/microSD in 1-bit SDMMC mode.
constexpr gpio_num_t SD_CLK = GPIO_NUM_39;
constexpr gpio_num_t SD_D0  = GPIO_NUM_40;
constexpr gpio_num_t SD_CMD = GPIO_NUM_41;

// Onboard BOOT button: active LOW, internal pull-up enabled.
// Hold during reset only for flashing; normal UI input after firmware startup.
constexpr gpio_num_t APP_BUTTON = GPIO_NUM_0;

// Battery divider is present on the board; not yet used by the MVP firmware.
constexpr gpio_num_t BAT_ADC = GPIO_NUM_4;

} // namespace board
