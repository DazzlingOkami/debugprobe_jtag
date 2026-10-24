/**
 * @file    mcp4716.h
 * @brief   MCP4716A0 reference voltage DAC driver, providing the reference voltage for the interface buffer chip.
 *
 * The device powers up by loading the DAC register and configuration bits from
 * EEPROM. Initialization only reads that state; voltage updates use the
 * write-volatile-memory command (010) and, when the setting must persist, the
 * write-all-memory command (011). Only the latter updates EEPROM, so settings
 * written with the volatile command are lost after power-off.
 *
 * The output voltage is VOUT = VREF * code / 1024 * GAIN.
 */

#ifndef _MCP4716_H
#define _MCP4716_H

#include <stdbool.h>
#include <stdint.h>
#include "dap_board.h"

/* Defaults used when the board configuration does not define them */

/** @brief 7-bit slave address; all address bits of the MCP4716A0 are 0 */
#ifndef DAC_MCP4716_I2C_ADDR
#define DAC_MCP4716_I2C_ADDR        (0x60)
#endif

/** @brief VREF pin voltage, in millivolts */
#ifndef DAC_MCP4716_VREF_MV
#define DAC_MCP4716_VREF_MV         (3300)
#endif

/** @brief Power-down pull-down selection: 1 = 1 kOhm, 2 = 125 kOhm, 3 = 640 kOhm */
#ifndef DAC_MCP4716_PD_RESISTOR
#define DAC_MCP4716_PD_RESISTOR     (3)
#endif

/** @brief Reference voltage source: 0 = VDD (unbuffered), 1 = VREF (unbuffered), 2 = VREF (buffered) */
#ifndef DAC_MCP4716_VREF_SEL
#define DAC_MCP4716_VREF_SEL        (2)
#endif

/** @brief Gain selection: 1 = 1x, 2 = 2x (2x is valid only on the VREF pin) */
#ifndef DAC_MCP4716_GAIN
#define DAC_MCP4716_GAIN            (1)
#endif

#if (DAC_MCP4716_VREF_MV <= 0)
#error "DAC_MCP4716_VREF_MV must be positive"
#endif

#if ((DAC_MCP4716_GAIN != 1) && (DAC_MCP4716_GAIN != 2))
#error "DAC_MCP4716_GAIN must be 1 or 2"
#endif

#if ((DAC_MCP4716_VREF_SEL < 0) || (DAC_MCP4716_VREF_SEL > 2))
#error "DAC_MCP4716_VREF_SEL must be 0, 1 or 2"
#endif

#if ((DAC_MCP4716_PD_RESISTOR < 1) || (DAC_MCP4716_PD_RESISTOR > 3))
#error "DAC_MCP4716_PD_RESISTOR must be 1, 2 or 3"
#endif

/** @brief Full-scale code value of the 10-bit data; larger code values are output at full scale */
#define DAC_MCP4716_CODE_MAX        (1023U)

/**
 * @brief  Initialize the driver: read the volatile DAC register and configuration bits.
 * @return 0 on success; -2 if the device does not respond
 * @note   Must be called after iic_shard_init(); no register is written. The device
 *         has already loaded its state from EEPROM when it powers up. Repeated calls
 *         return success immediately.
 */
int dac_mcp4716_init(void);

/**
 * @brief  Set the output voltage, or enter power-down mode.
 * @param  [in] mv             Target voltage in millivolts. 0 enters power-down mode
 *                             with the pull-down selected by DAC_MCP4716_PD_RESISTOR
 *                             while keeping the current code value.
 * @param  [in] save_to_eeprom true to also store the configuration and code in EEPROM
 * @return 0 on success; -1 if the device has not been initialized or the IIC write fails
 * @note   Values above full scale are clamped. With save_to_eeprom the call waits for
 *         the EEPROM write cycle (about 50 ms) before returning; EEPROM endurance is
 *         limited, so avoid changing the saved voltage frequently.
 */
int dac_mcp4716_set_mv(uint32_t mv, bool save_to_eeprom);

/**
 * @brief  Get the current output voltage.
 * @return In millivolts; 0 when the output is powered down or the driver is not initialized
 */
uint16_t dac_mcp4716_get_mv(void);

#endif /* _MCP4716_H */
