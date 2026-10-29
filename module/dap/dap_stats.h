/*
 * SPDX-License-Identifier: MIT
 */

/**
 * @file    dap_stats.h
 * @brief   DAP endpoint throughput counters used for diagnostics.
 *
 * The counters are maintained by the endpoint handler and read through the
 * vendor command 0x82. They help to tell whether the host keeps several
 * packets in flight and how many responses the device sends per second.
 * Set DAP_STATS_ENABLE to 1 to compile them in.
 */

#ifndef DAP_STATS_H
#define DAP_STATS_H

#include <stdint.h>

#include "DAP_config.h"

#if DAP_STATS_ENABLE

/** @brief Snapshot of the DAP endpoint statistics. */
typedef struct {
    uint32_t out_packets;    /**< Completed OUT transfers (requests) */
    uint32_t in_packets;     /**< Completed IN transfers (responses) */
    uint32_t uptime_ms;      /**< Tick counter when the snapshot was taken, 1 ms per tick */
    uint32_t busy_us;        /**< Total microseconds spent executing DAP commands */
    uint32_t max_busy_us;    /**< Longest single command execution time in microseconds */
    uint32_t min_out_gap_us; /**< Shortest interval between OUT completions, 0 when unknown */
    uint32_t max_out_gap_us; /**< Longest interval between OUT completions */
    uint32_t swclk_khz;      /**< SWCLK frequency currently in effect, in kHz */
    uint8_t max_req_depth;   /**< Deepest request ring occupancy seen so far */
    uint8_t max_resp_depth;  /**< Deepest response ring occupancy seen so far */
    uint8_t req_depth;       /**< Request ring occupancy at snapshot time */
    uint8_t resp_depth;      /**< Response ring occupancy at snapshot time */
} dap_stats_t;

/**
 * @brief  Take a consistent snapshot of the endpoint statistics.
 * @param  [out] stats Destination, must not be NULL
 */
void dap_stats_get(dap_stats_t *stats);

/** @brief Clear the packet counters and the recorded maximum depths. */
void dap_stats_reset(void);

#endif /* DAP_STATS_ENABLE */

#endif
