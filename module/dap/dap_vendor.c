/*
 * SPDX-License-Identifier: MIT
 */

/**
 * @file    dap_vendor.c
 * @brief   Project-specific CMSIS-DAP vendor commands.
 *
 * Vendor command 0x80 sets the reference voltage of the interface buffer and
 * 0x81 reports the current setting. The voltage is given in units of 0.1 V
 * (for example 33 = 3.3 V); 0 disables the DAC output. The setting is stored
 * in the DAC EEPROM so it is restored on the next power-up.
 */

#include <stdbool.h>
#include <stdint.h>

#include "DAP.h"
#include "dac_mcp4716/mcp4716.h"

/* Command IDs; OpenOCD sends them as "cmsis-dap cmd 0x80 <value>" */
#define DAP_VENDOR_CMD_VREF_SET         ID_DAP_Vendor0
#define DAP_VENDOR_CMD_VREF_GET         ID_DAP_Vendor1

/* Accepted voltage range in 0.1 V units; 0 is handled separately as "output off" */
#define DAP_VENDOR_VREF_MIN_DV          (12U)
#define DAP_VENDOR_VREF_MAX_DV          (33U)
#define DAP_VENDOR_VREF_OFF_DV          (0U)

/* One voltage unit is 0.1 V */
#define DAP_VENDOR_MV_PER_DV            (100U)

/* Result codes carried in the third response byte */
#define DAP_VENDOR_RESULT_OK            (0x00U)
#define DAP_VENDOR_RESULT_INVALID       (0x01U)
#define DAP_VENDOR_RESULT_HW_ERROR      (0x02U)

/* Response layout: [cmd, vref_10x, result, mv_lo, mv_hi] */
#define DAP_VENDOR_RESP_LEN             (5U)

/**
 * @brief  Build the common vendor response.
 * @param  [in]  cmd      Command ID copied to the first response byte
 * @param  [in]  req_len  Request length in bytes
 * @param  [in]  result   Result code
 * @param  [out] response Response buffer, at least DAP_VENDOR_RESP_LEN bytes
 * @return Request length in the upper 16 bits and response length in the lower 16 bits
 */
static uint32_t vref_response(uint8_t cmd, uint8_t req_len, uint8_t result, uint8_t *response)
{
    uint16_t mv = dac_mcp4716_get_mv();
    uint8_t dv = (uint8_t)((mv + (DAP_VENDOR_MV_PER_DV / 2U)) / DAP_VENDOR_MV_PER_DV);

    response[0] = cmd;
    response[1] = dv;
    response[2] = result;
    response[3] = (uint8_t)(mv & 0xFFU);
    response[4] = (uint8_t)(mv >> 8);

    return ((uint32_t)req_len << 16) | DAP_VENDOR_RESP_LEN;
}

/**
 * @brief  Handle the VREF set command and save the setting to EEPROM.
 * @param  [in]  value    Target voltage in 0.1 V units, 0 turns the output off
 * @param  [out] response Response buffer
 * @return Request and response length
 */
static uint32_t vref_set(uint8_t value, uint8_t *response)
{
    /* Reject out-of-range values before touching the DAC */
    if ((value != DAP_VENDOR_VREF_OFF_DV) &&
        ((value < DAP_VENDOR_VREF_MIN_DV) || (value > DAP_VENDOR_VREF_MAX_DV))) {
        return vref_response(DAP_VENDOR_CMD_VREF_SET, 2U, DAP_VENDOR_RESULT_INVALID, response);
    }

    if (dac_mcp4716_init() != 0) {
        return vref_response(DAP_VENDOR_CMD_VREF_SET, 2U, DAP_VENDOR_RESULT_HW_ERROR, response);
    }

    if (dac_mcp4716_set_mv((uint32_t)value * DAP_VENDOR_MV_PER_DV, true) != 0) {
        return vref_response(DAP_VENDOR_CMD_VREF_SET, 2U, DAP_VENDOR_RESULT_HW_ERROR, response);
    }

    return vref_response(DAP_VENDOR_CMD_VREF_SET, 2U, DAP_VENDOR_RESULT_OK, response);
}

/**
 * @brief  Handle the VREF get command.
 * @param  [out] response Response buffer
 * @return Request and response length
 */
static uint32_t vref_get(uint8_t *response)
{
    if (dac_mcp4716_init() != 0) {
        return vref_response(DAP_VENDOR_CMD_VREF_GET, 1U, DAP_VENDOR_RESULT_HW_ERROR, response);
    }

    return vref_response(DAP_VENDOR_CMD_VREF_GET, 1U, DAP_VENDOR_RESULT_OK, response);
}

/**
 * @brief  Process a DAP vendor command and prepare the response data.
 * @param  [in]  request  Pointer to the request data
 * @param  [out] response Pointer to the response data
 * @return Number of bytes in the response (lower 16 bits) and number of bytes in the request (upper 16 bits)
 */
uint32_t DAP_ProcessVendorCommand(const uint8_t *request, uint8_t *response)
{
    switch (request[0]) {
    case DAP_VENDOR_CMD_VREF_SET:
        return vref_set(request[1], response);

    case DAP_VENDOR_CMD_VREF_GET:
        return vref_get(response);

    default:
        *response = ID_DAP_Invalid;
        return (1U << 16) | 1U;
    }
}
