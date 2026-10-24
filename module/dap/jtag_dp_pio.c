/*
 * Copyright (c) 2013-2017 ARM Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file jtag_dp_pio.c
 * @brief PIO implementation of JTAG_DP: PIO bit streams replace bit-banged GPIO toggling
 * @note Mirrors sw_dp_pio.c; lib/CMakeLists.txt uses it in place of the upstream JTAG_DP.c
 */

#include <stdio.h>
#include <string.h>

#include "DAP_config.h"
#include "DAP.h"
#include "swd/probe.h"
#include "dap_pio.h"

/* TAP state navigation: Select-DR/Select-IR take two TMS=1 clocks, Capture takes two TMS=0 clocks */
static inline void jtag_nav(bool dr)
{
    if (dr) {
        jtag_tms_seq(3u, 0x1u);                     /* 1,0,0: Select-DR-Scan, Capture-DR, Shift-DR */
    } else {
        jtag_tms_seq(4u, 0x3u);                     /* 1,1,0,0: Select-DR, Select-IR, Capture-IR, Shift-IR */
    }
}

/* Fill n clocks with TDI=1 (bypass devices); n may be 0 */
static inline void jtag_ones(uint32_t n)
{
    while (n != 0u) {
        const uint32_t chunk = (n > 32u) ? 32u : n;
        jtag_bits(chunk, false, 0xFFFFFFFFu, false);
        n -= chunk;
    }
}

/* Fill n clocks with TDI=0; n may be 0 */
static inline void jtag_zeros(uint32_t n)
{
    while (n != 0u) {
        const uint32_t chunk = (n > 32u) ? 32u : n;
        jtag_bits(chunk, false, 0u, false);
        n -= chunk;
    }
}

/* Clock one TCK with TMS = tms and TDI = tdi */
static inline void jtag_bit(uint32_t tdi, bool tms)
{
    jtag_bits(1u, tms, tdi, false);
}

/* n clocks with fixed TMS and TDI=0; n may be 0. The count field is 7 bits, so longer runs are split */
static inline void jtag_clocks(uint32_t n, bool tms)
{
    if (n == 0u) {
        return;
    }

    jtag_tms(tms ? 1u : 0u);
    while (n != 0u) {
        const uint32_t chunk = (n > PROBE_CMD_MAX_BITS) ? PROBE_CMD_MAX_BITS : n;
        probe_write_bits(chunk, 0u);
        n -= chunk;
    }
}

/* Update-DR/IR and Idle: one TMS=1 clock back to Update, then one TMS=0 clock back to Idle */
static inline void jtag_update_idle(void)
{
    jtag_tms_seq(2u, 0x1u);
}

// Generate JTAG Sequence
//   info:   sequence information
//   tdi:    pointer to TDI generated data
//   tdo:    pointer to TDO captured data
//   return: none
void JTAG_Sequence (uint32_t info, const uint8_t *tdi, uint8_t *tdo) {
  uint32_t tdi_word[2] = { 0u, 0u };
  uint32_t tdo_word[2] = { 0u, 0u };
  uint32_t n;
  uint32_t lo_bits;
  bool tms;
  bool capture;

  n = info & JTAG_SEQUENCE_TCK;
  if (n == 0U) {
    n = 64U;
  }
  tms = (info & JTAG_SEQUENCE_TMS) != 0U;
  capture = ((info & JTAG_SEQUENCE_TDO) != 0U) && (tdo != NULL);
  probe_debug("JTAG sequence %d bits tms %d capture %d\n", n, tms, capture);

  /* Up to 64 bits, split into one or two 32-bit words; low bits are sent first (little-endian) */
  lo_bits = (n > 32U) ? 32U : n;

  if (tdi != NULL) {
    memcpy(&tdi_word[0], tdi, (lo_bits + 7U) >> 3);
    if (n > 32U) {
      memcpy(&tdi_word[1], &tdi[4], ((n - 32U) + 7U) >> 3);
    }
  }

  tdo_word[0] = jtag_bits(lo_bits, tms, tdi_word[0], capture);
  if (n > 32U) {
    tdo_word[1] = jtag_bits(n - 32U, tms, tdi_word[1], capture);
  }

  if (capture) {
    memcpy(tdo, tdo_word, (n + 7U) >> 3);
  }
}

// Generate SWJ Sequence in JTAG mode
//   count:  sequence bit count
//   data:   pointer to sequence bit data (TMS levels, LSB first)
//   return: none
#if (DAP_JTAG != 0)
void JTAG_SWJ_Sequence (uint32_t count, const uint8_t *data) {
  uint32_t pos;

  /* Each run of equal TMS levels becomes one scan; pos is both the bit offset and the run start */
  pos = 0;
  while (pos < count) {
    const uint32_t tms = ((uint32_t)data[pos >> 3] >> (pos & 7u)) & 1u;
    uint32_t run = 1;

    while (((pos + run) < count) &&
           ((((uint32_t)data[(pos + run) >> 3] >> ((pos + run) & 7u)) & 1u) == tms)) {
      run++;
    }
    if (run <= 8u) {
      uint32_t left = run;

      /* One TMS sequence command spans at most 4 clocks, so longer equal-level runs are split further */
      while (left != 0u) {
        const uint32_t chunk = (left > PROBE_TMS_SEQ_MAX) ? PROBE_TMS_SEQ_MAX : left;
        jtag_tms_seq(chunk, tms ? ((1u << chunk) - 1u) : 0u);
        left -= chunk;
      }
    } else {
      jtag_clocks(run, tms != 0u);
    }
    pos += run;
  }
}
#endif

// JTAG Set IR
//   ir:     IR value
//   return: none
void JTAG_IR (uint32_t ir) {
  uint32_t before;
  uint32_t length;
  uint32_t after;

  before = DAP_Data.jtag_dev.ir_before[DAP_Data.jtag_dev.index];
  length = DAP_Data.jtag_dev.ir_length[DAP_Data.jtag_dev.index];
  after  = DAP_Data.jtag_dev.ir_after [DAP_Data.jtag_dev.index];

  jtag_nav(false);                                  /* Select-DR, Select-IR, Capture-IR, Shift-IR */
  jtag_ones(before);                                /* Leading bypass bits: TDI=1, TMS=0 */

  if (length != 0U) {
    jtag_bits(length - 1U, false, ir, false);       /* IR low bits, LSB first */
  }
  jtag_bit((length ? (ir >> (length - 1U)) : 0U) & 1U, true);   /* Last bit shares the clock with Exit1-IR */

  if (after != 0U) {
    jtag_zeros(after - 1U);                         /* Trailing bypass bits, excluding the last clock */
    jtag_bit(1U, true);                             /* Last clock: Exit1-IR */
  }
  jtag_update_idle();
}

// JTAG Read IDCODE register
//   return: value read
uint32_t JTAG_ReadIDCode (void) {
  uint32_t n;
  uint32_t val;

  jtag_nav(true);                                   /* Select-DR-Scan, Capture-DR, Shift-DR */
  jtag_zeros(DAP_Data.jtag_dev.index);              /* Leading bypass bits */

  /* The last bit shares the clock with Exit1-DR, so only 31 clocks use TMS=0 */
  val = jtag_bits(31u, false, 0u, true);
  n = DAP_Data.jtag_dev.count - DAP_Data.jtag_dev.index - 1U;
  if (n != 0U) {
    jtag_bit(0u, false);                            /* D31 */
    jtag_zeros(n - 1U);                             /* Trailing bypass bits */
    jtag_bit(1u, true);                             /* Exit1-DR */
  } else {
    val |= jtag_bits(1u, true, 0u, true) << 31;     /* D31 shares the clock with Exit1-DR */
  }
  jtag_update_idle();

  return val;
}

// JTAG Write ABORT register
//   data:   value to write
//   return: none
void JTAG_WriteAbort (uint32_t data) {
  uint32_t n;

  jtag_nav(true);                                   /* Select-DR-Scan, Capture-DR, Shift-DR */
  jtag_zeros(DAP_Data.jtag_dev.index);              /* Leading bypass bits */

  jtag_zeros(3u);                                   /* RnW=0, A2=0, A3=0 */

  jtag_bits(31u, false, data, false);               /* D0..D30 shifted out by a single command */
  data >>= 31;
  n = DAP_Data.jtag_dev.count - DAP_Data.jtag_dev.index - 1U;
  if (n != 0U) {
    jtag_bit(data & 1u, false);                     /* D31 */
    jtag_zeros(n - 1U);                             /* Trailing bypass bits */
    jtag_bit(1u, true);                             /* Exit1-DR */
  } else {
    jtag_bit(data & 1u, true);                      /* D31 shares the clock with Exit1-DR */
  }
  jtag_update_idle();
}

/* Read data phase: 32 data bits (last bit on the Exit1-DR clock) plus trailing bypass; returns the data read */
static inline uint32_t jtag_read_data (uint32_t n_after) {
  /* Only 31 clocks use TMS=0, as D31 shares the Exit1-DR clock; the bit captured there is discarded */
  uint32_t val = jtag_bits(31u, false, 0u, true);

  jtag_bits(1u, true, 0u, true);
  jtag_zeros(n_after);                              /* Trailing bypass bits */
  return val;
}

/* Write data phase: 32 data bits (last bit on the Exit1-DR clock) plus trailing bypass */
static inline void jtag_write_data (uint32_t val, uint32_t n_after) {
  jtag_bits(31u, false, val, false);                /* D0..D30 shifted out by a single command */
  jtag_bit((val >> 31) & 1u, true);                 /* D31 shares the clock with Exit1-DR */
  jtag_zeros(n_after);                              /* Trailing bypass bits */
}

// JTAG Transfer I/O
//   request: A[3:2] RnW APnDP
//   data:    DATA[31:0]
//   return:  ACK[2:0]
uint8_t JTAG_Transfer (uint32_t request, uint32_t *data) {
  uint32_t ack;
  uint32_t n_after;
  uint32_t val;

  probe_debug("JTAG transfer\n");

  /* Select-DR-Scan, Capture-DR, Shift-DR */
  jtag_nav(true);

  jtag_zeros(DAP_Data.jtag_dev.index);              /* Leading bypass bits */

  /* Three clocks shift out RnW/A2/A3 and capture ACK.0/1/2 on the same clocks; bit0 is ACK.0 */
  ack = jtag_bits(3u, false, request >> 1, true) & 0x7u;

  if (ack == DAP_TRANSFER_OK) {
    n_after = DAP_Data.jtag_dev.count - DAP_Data.jtag_dev.index - 1U;
    if (request & DAP_TRANSFER_RnW) {
      val = jtag_read_data(n_after);
      if (data != NULL) {
        *data = val;
      }
      probe_debug("Read ack %02x 0x%08x\n", ack, val);
    } else {
      val = (data != NULL) ? *data : 0u;
      jtag_write_data(val, n_after);
      probe_debug("Write ack %02x 0x%08x\n", ack, val);
    }
  } else {
    /* Exit1-DR straight away on error */
    jtag_bit(1u, true);
  }

  jtag_update_idle();

  /* Capture Timestamp */
  if (request & DAP_TRANSFER_TIMESTAMP) {
    DAP_Data.timestamp = TIMESTAMP_GET();
  }

  /* Idle cycles: TMS=0, TDI=0 */
  jtag_clocks(DAP_Data.transfer.idle_cycles, false);

  /* A PIO hand-off timeout leaves the bit stream of this transfer corrupt: report it as an
   * error instead of handing back data that was never clocked */
  return probe_take_timeout() ? (uint8_t)DAP_TRANSFER_ERROR : (uint8_t)ack;
}
