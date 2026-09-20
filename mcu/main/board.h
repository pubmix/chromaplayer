#pragma once

// MCU masters QSPI_ to FPGA
#define PIN_NUM_QSPI_CS          GPIO_NUM_5 // D10
#define PIN_NUM_QSPI_CLK         GPIO_NUM_18 // D9
#define PIN_NUM_QSPI_MOSI        GPIO_NUM_23 // D8
#define PIN_NUM_QSPI_MISO        GPIO_NUM_19 // D7
#define PIN_NUM_QSPI_WP          GPIO_NUM_22 // D6
#define PIN_NUM_QSPI_HD          GPIO_NUM_21 // D5

// I2S
#define PIN_NUM_I2S_BCLK    GPIO_NUM_33 //D16
#define PIN_NUM_I2S_WS      GPIO_NUM_25 //D15
#define PIN_NUM_I2S_DIN     GPIO_NUM_26 //D14
#define PIN_NUM_I2S_DOUT    GPIO_NUM_27 //D13

// Native ESP32 SDMMC slot 1. The schematic routes all four data lines directly
// between the ESP32 and J7. The USB-UART bridge uses GPIO35/36, not GPIO4.
#define PIN_NUM_SD_CLK      GPIO_NUM_14
#define PIN_NUM_SD_CMD      GPIO_NUM_15
#define PIN_NUM_SD_D0       GPIO_NUM_2
#define PIN_NUM_SD_D1       GPIO_NUM_4
#define PIN_NUM_SD_D2       GPIO_NUM_12
#define PIN_NUM_SD_D3       GPIO_NUM_13
#define PIN_NUM_SD_DETECT   GPIO_NUM_39

// GPIO27 is otherwise unused by the stock firmware. The private FPGA image
// interprets it as an active-high request to route MCU I2S into the codec.
#define PIN_NUM_AUDIO_ROUTE PIN_NUM_I2S_DOUT

// Low latency low throughput async uart
#define PIN_NUM_UART_FROM_FPGA  GPIO_NUM_10 //D11
#define PIN_NUM_UART_TO_FPGA    GPIO_NUM_9 //D12
