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
 *
 * Vendor command 0x83 is a throughput probe: it eats one full-size request
 * packet without inspecting the payload and returns a short response, so a
 * host tool can measure the bulk path on its own.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "DAP_config.h"
#include "DAP.h"
#include "dac_mcp4716/mcp4716.h"
#include "dap_stats.h"

/* Command IDs; OpenOCD sends them as "cmsis-dap cmd 0x80 <value>" */
#define DAP_VENDOR_CMD_VREF_SET         ID_DAP_Vendor0
#define DAP_VENDOR_CMD_VREF_GET         ID_DAP_Vendor1
#define DAP_VENDOR_CMD_STATS            ID_DAP_Vendor2
#define DAP_VENDOR_CMD_BENCH            ID_DAP_Vendor3

/* Benchmark command: one full-size request packet in, a short response out.
 * Response length is selected by the host between these bounds. */
#define DAP_VENDOR_BENCH_MIN_RESP       (4U)
#define DAP_VENDOR_BENCH_DEFAULT_RESP   (4U)
#define DAP_VENDOR_BENCH_STATUS_OK      (0x00U)

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

#if DAP_STATS_ENABLE
/* Statistics selector carried in the second request byte */
#define DAP_VENDOR_STATS_DEPTH          (0U)    /**< Max/current ring depth */
#define DAP_VENDOR_STATS_OUT            (1U)    /**< Completed OUT transfers */
#define DAP_VENDOR_STATS_IN             (2U)    /**< Completed IN transfers */
#define DAP_VENDOR_STATS_UPTIME         (3U)    /**< Tick counter in ms */
#define DAP_VENDOR_STATS_RESET          (4U)    /**< Clear all counters */
#define DAP_VENDOR_STATS_BUSY           (5U)    /**< Total command execution time in us */
#define DAP_VENDOR_STATS_MAX_BUSY       (6U)    /**< Longest command execution in us */
#define DAP_VENDOR_STATS_MIN_GAP        (7U)    /**< Shortest request interval in us */
#define DAP_VENDOR_STATS_MAX_GAP        (8U)    /**< Longest request interval in us */
#define DAP_VENDOR_STATS_SWCLK          (9U)    /**< Actual SWCLK in kHz */

/* Returned in the first data byte for an unknown selector */
#define DAP_VENDOR_STATS_INVALID        (0xFFU)

/* Statistics response layout: [cmd, b0, b1, b2, b3] */
#define DAP_VENDOR_STATS_RESP_LEN       (5U)
#endif

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

#if DAP_STATS_ENABLE
/**
 * @brief  Encode a 32-bit value as little-endian bytes.
 * @param  [out] dst   Destination, 4 bytes
 * @param  [in]  value Value to encode
 */
static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/**
 * @brief  Handle the endpoint statistics command.
 * @param  [in]  sel      Selector: depth, counters, uptime or reset
 * @param  [out] response Response buffer, at least DAP_VENDOR_STATS_RESP_LEN bytes
 * @return Request and response length
 */
static uint32_t stats_command(uint8_t sel, uint8_t *response)
{
    dap_stats_t stats;

    response[0] = DAP_VENDOR_CMD_STATS;

    if (sel == DAP_VENDOR_STATS_RESET) {
        dap_stats_reset();
        response[1] = 0;
        response[2] = 0;
        response[3] = 0;
        response[4] = 0;
        return (2U << 16) | DAP_VENDOR_STATS_RESP_LEN;
    }

    dap_stats_get(&stats);

    switch (sel) {
    case DAP_VENDOR_STATS_DEPTH:
        response[1] = stats.max_req_depth;
        response[2] = stats.max_resp_depth;
        response[3] = stats.req_depth;
        response[4] = stats.resp_depth;
        break;

    case DAP_VENDOR_STATS_OUT:
        put_u32_le(&response[1], stats.out_packets);
        break;

    case DAP_VENDOR_STATS_IN:
        put_u32_le(&response[1], stats.in_packets);
        break;

    case DAP_VENDOR_STATS_UPTIME:
        put_u32_le(&response[1], stats.uptime_ms);
        break;

    case DAP_VENDOR_STATS_BUSY:
        put_u32_le(&response[1], stats.busy_us);
        break;

    case DAP_VENDOR_STATS_MAX_BUSY:
        put_u32_le(&response[1], stats.max_busy_us);
        break;

    case DAP_VENDOR_STATS_MIN_GAP:
        put_u32_le(&response[1], stats.min_out_gap_us);
        break;

    case DAP_VENDOR_STATS_MAX_GAP:
        put_u32_le(&response[1], stats.max_out_gap_us);
        break;

    case DAP_VENDOR_STATS_SWCLK:
        put_u32_le(&response[1], stats.swclk_khz);
        break;

    default:
        response[1] = DAP_VENDOR_STATS_INVALID;
        response[2] = 0;
        response[3] = 0;
        response[4] = 0;
        break;
    }

    return (2U << 16) | DAP_VENDOR_STATS_RESP_LEN;
}
#endif

/**
 * @brief  Consume one full-size request packet and return a short response.
 * @param  [in]  request  Request buffer; only the sequence and length selector are read
 * @param  [out] response Response buffer, at least DAP_PACKET_SIZE bytes
 * @return Request length in the upper 16 bits and response length in the lower 16 bits
 */
static uint32_t bench_command(const uint8_t *request, uint8_t *response)
{
    uint8_t len = request[3];

    if (len < DAP_VENDOR_BENCH_MIN_RESP) {
        len = DAP_VENDOR_BENCH_DEFAULT_RESP;
    } else if (len > DAP_PACKET_SIZE) {
        len = DAP_PACKET_SIZE;
    }

    /* The payload is ignored on purpose: only the USB path is under test */
    memset(response, 0, len);
    response[0] = DAP_VENDOR_CMD_BENCH;
    response[1] = request[1];
    response[2] = request[2];
    response[3] = DAP_VENDOR_BENCH_STATUS_OK;

    return ((uint32_t)DAP_PACKET_SIZE << 16) | len;
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

    case DAP_VENDOR_CMD_BENCH:
        return bench_command(request, response);

#if DAP_STATS_ENABLE
    case DAP_VENDOR_CMD_STATS:
        return stats_command(request[1], response);
#endif

    default:
        *response = ID_DAP_Invalid;
        return (1U << 16) | 1U;
    }
}
