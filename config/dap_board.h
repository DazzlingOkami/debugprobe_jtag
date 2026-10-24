#ifndef _BOARDS_CMLINK_V3_H
#define _BOARDS_CMLINK_V3_H
#include "pico.h"

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define PICO_BOARD_DAP_BOARD

// --- Debug UART ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 1
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 20
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 25 // Not usaed
#endif

#define PICO_RP2350A 1              // 0 = RP2350B, 1 = RP2350A
#define PICO_PIO_USE_GPIO_BASE 0

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64

#define PROBE_CDC_UART

// SWD PIO config
#define PROBE_PIO_INTERFACE     (pio0)
#define PROBE_SM                0
#define PROBE_JTAG_TDI_PIN      (0)
#define PROBE_JTAG_TDO_PIN      (2)
#define PROBE_SWD_CLK_PIN       (3)
#define PROBE_SWD_DOE_PIN       (4)
#define PROBE_SWD_DI_PIN        (5)
#define PROBE_SWD_DO_PIN        (6)
#define PROBE_RESET_PIN         (7)
#define PROBE_JTAG_TRST_PIN     (9)

// UART config
/* CDC bridge TX follows the debug mode: GPIO0 for SWD, GPIO12 for JTAG (GPIO0 is the PIO TDI pin) */
/* With GPIO12 as UART output, GPIO8 must be high so the external buffer drives out (low selects input) */
#define PROBE_UART_TX_SWD_PIN   (0)
#define PROBE_UART_TX_JTAG_PIN  (12)
#define PROBE_UART_TX_OE_PIN    (8)
#define PROBE_UART_RX           (1)
#define PROBE_UART_INTERFACE    uart0
#define PROBE_UART_BAUDRATE     115200

// IIC
#define IIC_INTERFACE           (i2c1)
#define IIC_SCL_PIN             (11)
#define IIC_SDA_PIN             (10)

// MCP4716A0: reference voltage for the interface buffer; runs from 4V, VREF tied to 3.3V
#define DAC_MCP4716_I2C_ADDR    (0x60)
#define DAC_MCP4716_VREF_MV     (3300)
/* Power-down pull-down: 1 = 1k, 2 = 125k, 3 = 640k to ground */
#define DAC_MCP4716_PD_RESISTOR (3)

/* Status LEDs: connect LED on GPIO21, running LED on GPIO22
 * LEDs sink current from 3V3 through a resistor, so a low level lights them */
#define PROBE_DAP_CONNECTED_LED (21)
#define PROBE_DAP_RUNNING_LED   (22)
#define PROBE_LED_ACTIVE_LEVEL  (0)

/* DAP_Info strings; must stay consistent with the strings in the USB descriptor */
#define PROBE_VENDOR_STRING     "Cassini"
#define PROBE_PRODUCT_STRING    "CMSIS_DAP"
#define PROBE_FIRMWARE_VERSION  "2.10"

#endif // _BOARDS_CMLINK_V3_H
