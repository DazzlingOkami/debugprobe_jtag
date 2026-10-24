/*
 * Copyright (c) 2013-2022 ARM Limited. All rights reserved.
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
 * @file dap_pio.h
 * @brief Internal interface shared between the PIO implementations of DAP
 */

#ifndef DAP_PIO_H_
#define DAP_PIO_H_

#include <stdint.h>

#include "DAP_config.h"

#if (DAP_JTAG != 0)
/** @brief Generate an SWJ switch sequence in JTAG mode (the data is a TMS level stream)
 * @param[in] count Number of bits
 * @param[in] data TMS level bit stream, LSB first
 * @note Called by SWJ_Sequence() in module/dap/sw_dp_pio.c in JTAG mode */
void JTAG_SWJ_Sequence(uint32_t count, const uint8_t *data);
#endif

#endif
