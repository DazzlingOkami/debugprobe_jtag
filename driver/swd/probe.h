/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

/**
 * @file probe.h
 * @brief SWD/JTAG bit-stream engine: PIO command dispatch interface
 */

#ifndef PROBE_H_
#define PROBE_H_

#include <stdbool.h>
#include <stdint.h>

#include "probe_oen.pio.h"

/** @brief Bit count limit of a single command (the count field is 7 bits wide) */
#define PROBE_CMD_MAX_BITS 128u

/** @brief Maximum number of cycles per command supported by jtag_tms_seq() */
#define PROBE_TMS_SEQ_MAX 4u

/** @brief Lowest SWD/JTAG clock the engine accepts, in kHz
 * @note Keeps the divider and the worst case hand-off time inside the limits of the
 *       16:8 divider and of the hand-off timeout; lower requests are raised to this value */
#define PROBE_SWCLK_MIN_KHZ 10u

/** @brief SWD/JTAG clock used when no frequency has been requested yet, in Hz */
#define PROBE_SWCLK_DEFAULT_HZ 1000000u

/** @brief Set the SWD clock frequency in Hz
 * @param[in] freq_hz Target frequency in Hz, raised to PROBE_SWCLK_MIN_KHZ * 1000 when lower */
void probe_set_swclk_freq_hz(uint32_t freq_hz);

/** @brief Set the SWD clock frequency in kHz, equivalent to probe_set_swclk_freq_hz()
 * @param[in] freq_khz Target frequency in kHz, raised to PROBE_SWCLK_MIN_KHZ when lower */
void probe_set_swclk_freq(uint freq_khz);

/** @brief Read the SWD clock frequency actually in effect
 * @return Actual frequency in kHz */
uint probe_get_swclk_freq(void);

/** @brief Set the TCK frequency, equivalent to probe_set_swclk_freq()
 * @param[in] freq_khz Target frequency in kHz, raised to PROBE_SWCLK_MIN_KHZ when lower */
void probe_set_tck_freq(uint32_t freq_khz);

/** @brief Read the TCK frequency actually in effect
 * @return Actual frequency in kHz, the same value as probe_get_swclk_freq() */
uint32_t probe_get_tck_freq(void);

/** @brief Check whether the state machine has drained its commands and is parked at the dispatch point
 * @return True when the PIO is idle and the pin binding can be changed safely
 * @note Based on TX FIFO empty plus PC at the dispatch point, not on FDEBUG TXSTALL */
bool probe_is_idle(void);

/** @brief Take the sticky hand-off timeout flag and clear it
 * @return True when a FIFO hand-off or the idle wait timed out since the last call, which means
 *         the bit stream of the current operation is corrupt and the result must be discarded
 * @note The flag is set by probe_put(), probe_get() and probe_wait_idle(); while it is set the
 *       queued commands are dropped, and the state machine is parked at the dispatch point again */
bool probe_take_timeout(void);

/** @brief Load the PIO program and initialize the state machine, idempotent */
void probe_init(void);

/** @brief Release the pins and the state machine */
void probe_deinit(void);

/** @brief Clock out n bits and drive data
 * @param[in] bit_count Bit count, 1..PROBE_CMD_MAX_BITS; wider transfers are split by the caller
 * @param[in] data_byte Data driven LSB first, the low bit_count bits are used; 0 is driven beyond 32 bits */
void probe_write_bits(uint bit_count, uint32_t data_byte);

/** @brief Merge command and data into a single FIFO write, bit width up to 20
 * @param[in] bit_count Bit count, 1..20
 * @param[in] data_byte Data driven LSB first */
void probe_write_short_bits(uint bit_count, uint32_t data_byte);

/** @brief Read n bits and return them right aligned
 * @param[in] bit_count Bit count, 1..128; beyond 32 bits only the last 32 bits are kept,
 *                      so it suits dummy reads only
 * @return Right-aligned sample value, bit0 is the bit captured on the first clock */
uint32_t probe_read_bits(uint bit_count);

/** @brief Generate n high-impedance clocks, split automatically above 20
 * @param[in] bit_count Number of clocks */
void probe_hiz_clocks(uint bit_count);

/** @brief Reserved interface: switch to the SWD read direction, currently empty
 * @note The direction is driven per cycle by the side-set (DOE) of the PIO commands,
 *       no software switch is needed */
void probe_read_mode(void);

/** @brief Reserved interface: switch to the SWD write direction, currently empty
 * @note The direction is driven per cycle by the side-set (DOE) of the PIO commands,
 *       no software switch is needed */
void probe_write_mode(void);

/** @brief Switch to the JTAG pin binding: TMS output, TDI output, TDO input
 * @note Calls probe_wait_idle() first, then sets the pin binding and directions */
void probe_enter_jtag(void);

/** @brief Switch to the SWD pin binding: SWD_DI input, SWD_DO output, TDI and TDO back to input */
void probe_enter_swd(void);

/** @brief Drive the JTAG TMS level without generating a clock
 * @param[in] tms TMS level */
void jtag_tms(uint32_t tms);

/** @brief Drive n clocked TMS cycles as a single FIFO command
 * @param[in] n Number of TCK cycles, 1..PROBE_TMS_SEQ_MAX; longer sequences are split by the caller
 * @param[in] tms TMS level sequence, bit0 is the TMS level of the first cycle, LSB first
 * @note Only for TAP cycles where TDI does not matter (navigation, Capture, Update, Idle, SWJ sequence),
 *       TDI keeps its previous level; nothing is done when n is out of range */
void jtag_tms_seq(uint32_t n, uint32_t tms);

/** @brief Drive n TCK cycles and sample TDO, bit width up to 32
 * @param[in] n Number of TCK cycles, 1..32
 * @param[in] tms Constant TMS level of this segment
 * @param[in] tdi Right-aligned TDI data, the low n bits are used (LSB first)
 * @param[in] capture When true the captured value is returned, otherwise 0 is returned
 * @return Captured value, bit0 is the TDO sampled on the first TCK; 0 when capture is false */
uint32_t jtag_bits(uint32_t n, bool tms, uint32_t tdi, bool capture);

void probe_assert_reset(bool state);
int probe_reset_level(void);

#endif
