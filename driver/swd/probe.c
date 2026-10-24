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
 * @file probe.c
 * @brief SWD/JTAG bit-stream engine: dispatch commands to the PIO state machine
 */

#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include "probe_config.h"
#include "probe.h"
#include "uart/cdc_uart.h"

struct _probe {
    // PIO offset
    uint offset;
    uint initted;
    /* Current pin binding: 0 = unbound, other values in probe_mode_t */
    uint mode;
    /* Timeout of one FIFO hand-off, derived from the divider in effect */
    uint32_t timeout_us;
    /* Set when a hand-off timed out; the command stream is discarded until it is taken */
    bool fault;
};

/* Pin binding mode, the values of probe.mode */
typedef enum probe_mode {
    PROBE_MODE_NONE = 0,
    PROBE_MODE_SWD,
    PROBE_MODE_JTAG,
} probe_mode_t;

static struct _probe probe;
static uint swclk_actual_freq;

/* Last requested frequency, re-applied when the state machine is initialized */
static uint32_t swclk_requested_hz = PROBE_SWCLK_DEFAULT_HZ;

/* JTAG binding: TMS is shared with SWD_DO, so set and out point to different pin groups */
#define PROBE_JTAG_IN_BASE   PROBE_JTAG_TDO_PIN
#define PROBE_JTAG_OUT_BASE  PROBE_JTAG_TDI_PIN
#define PROBE_JTAG_SET_BASE  PROBE_SWD_DO_PIN

/* Last TMS level driven; when valid is false the next JTAG operation must issue a TMS command first */
static uint8_t jtag_tms_level;
static bool jtag_tms_valid;

/* Pin directions are set with pio_sm_set_pindirs_with_mask(), which must only be called while the
 * state machine is stopped: the function works by injecting a set pindirs instruction into the SM,
 * and while the SM runs (or spins on pull with a large divider) the injected instruction executes
 * at an unpredictable time, so the direction is not applied reliably.
 * Mode switches therefore always perform: wait idle, stop SM, configure pins, start SM */
#define PROBE_PIN_OUTPUT(pin) pio_sm_set_pindirs_with_mask(pio0, PROBE_SM, (1u << (pin)), (1u << (pin)))
#define PROBE_PIN_INPUT(pin)  pio_sm_set_pindirs_with_mask(pio0, PROBE_SM, 0, (1u << (pin)))

static void probe_wait_idle(void);
static void probe_sm_stop(void);
static void probe_sm_start(void);
static void probe_fault_set(void);

/* One command bit occupies four state machine cycles (see probe_oen.pio), so a hand-off that
 * waits for the whole command queue is bounded by the divider currently in effect */
#define PROBE_SM_CYCLES_PER_BIT 4u
/* Worst case before a hand-off completes: a full queue, i.e. four commands of maximum width */
#define PROBE_SM_CYCLES_MAX (4u * PROBE_CMD_MAX_BITS * PROBE_SM_CYCLES_PER_BIT)
/* Margin over the worst case drain time, and the clamp of the resulting timeout */
#define PROBE_TIMEOUT_MARGIN 2u
#define PROBE_TIMEOUT_MIN_US 1000u
#define PROBE_TIMEOUT_MAX_US 500000u

/* Deadline of a hand-off wait, based on the current divider */
static inline uint64_t probe_deadline(void)
{
    return time_us_64() + probe.timeout_us;
}

/* Discard the queued commands and park the state machine at the dispatch point after a
 * hand-off timeout. The bit stream is no longer aligned with the command stream, so the whole
 * queue is dropped instead of leaving the SM stalled on a pull that can never complete */
static void probe_fault_set(void)
{
    probe.fault = true;
    probe_sm_stop();
    pio_sm_clear_fifos(pio0, PROBE_SM);
    pio_sm_exec(pio0, PROBE_SM, probe.offset + probe_offset_get_next_cmd);
    probe_sm_start();
    /* The TMS level of the interrupted stream is unknown */
    jtag_tms_valid = false;
}

/* Write the TX FIFO: spin with a timeout while the FIFO is full, normally enqueued on the first try */
static void probe_put(uint32_t word)
{
    if(probe.fault){
        /* The stream is not aligned: drop the command until the caller takes the fault */
        return;
    }

    if(pio_sm_is_tx_fifo_full(pio0, PROBE_SM)){
        const uint64_t deadline = probe_deadline();

        while(pio_sm_is_tx_fifo_full(pio0, PROBE_SM)){
            if(time_us_64() >= deadline){
                probe_fault_set();
                return;
            }
        }
    }
    pio_sm_put(pio0, PROBE_SM, word);
}

/* Read the RX FIFO: spin with a timeout while it is empty. Exceeding the limit means the SM never
 * reached push, so the data is meaningless and the fault is latched instead of being returned */
static uint32_t probe_get(void)
{
    if(probe.fault){
        return 0u;
    }

    if(pio_sm_is_rx_fifo_empty(pio0, PROBE_SM)){
        const uint64_t deadline = probe_deadline();

        while(pio_sm_is_rx_fifo_empty(pio0, PROBE_SM)){
            if(time_us_64() >= deadline){
                probe_fault_set();
                return 0u;
            }
        }
    }
    return pio_sm_get(pio0, PROBE_SM);
}

void probe_set_swclk_freq_hz(uint32_t freq_hz) {
    const uint32_t clk_sys_hz = clock_get_hz(clk_sys);

    /* Below the minimum the divider exceeds the longest hand-off the timeout covers, and a zero
     * frequency would produce an infinite divider */
    if(freq_hz < (PROBE_SWCLK_MIN_KHZ * 1000u)){
        freq_hz = PROBE_SWCLK_MIN_KHZ * 1000u;
    }
    swclk_requested_hz = freq_hz;

    /* divider = clk_sys / (4 * freq), held in 1/256 units and rounded up so the actual frequency
     * never exceeds the request; 64 * clk_sys / freq fits in 64 bits */
    uint64_t divider_256 = ((uint64_t)clk_sys_hz * 64u + freq_hz - 1u) / freq_hz;
    if(divider_256 < 256u){
        divider_256 = 256u;
    }
    /* The divider field is 16:8, an integer part of zero encodes 65536 */
    if(divider_256 > (65536u * 256u)){
        divider_256 = 65536u * 256u;
    }

    uint16_t div_int = (uint16_t)(divider_256 >> 8);
    uint8_t div_frac = (uint8_t)(divider_256 & 0xFFu);
    if(divider_256 == (65536u * 256u)){
        div_int = 0u;
        div_frac = 0u;
    }
    pio_sm_set_clkdiv_int_frac(pio0, PROBE_SM, div_int, div_frac);

    /* swclk = clk_sys / (divider * 4) = clk_sys * 64 / divider_256 */
    swclk_actual_freq = (uint32_t)(((uint64_t)clk_sys_hz * 64u) / divider_256 / 1000u);

    /* One hand-off may wait for the whole queue: cycles * divider / clk_sys, with margin */
    uint32_t timeout_us = (uint32_t)(((uint64_t)PROBE_TIMEOUT_MARGIN * PROBE_SM_CYCLES_MAX *
                                      divider_256 * 1000000ull) /
                                     ((uint64_t)256u * clk_sys_hz));
    if(timeout_us < PROBE_TIMEOUT_MIN_US){
        timeout_us = PROBE_TIMEOUT_MIN_US;
    }else if(timeout_us > PROBE_TIMEOUT_MAX_US){
        timeout_us = PROBE_TIMEOUT_MAX_US;
    }
    probe.timeout_us = timeout_us;
}

void probe_set_swclk_freq(uint freq_khz) {
    probe_set_swclk_freq_hz(freq_khz * 1000u);
}

uint probe_get_swclk_freq(void){
    return swclk_actual_freq;
}

void probe_set_tck_freq(uint32_t freq_khz){
    probe_set_swclk_freq(freq_khz);
}

uint32_t probe_get_tck_freq(void){
    return swclk_actual_freq;
}

/* Stop/start the state machine: the protected region for pin direction changes during mode
 * switches. PROBE_PIN_OUTPUT/INPUT inject a set pindirs instruction into the SM, and only a
 * stopped SM executes it immediately, which makes the direction take effect reliably */
static void probe_sm_stop(void){
    pio_sm_set_enabled(pio0, PROBE_SM, false);
}

static void probe_sm_start(void){
    pio_sm_set_enabled(pio0, PROBE_SM, true);
}

static void probe_set_jtag_pins(void){
    pio_sm_set_in_pins(pio0, PROBE_SM, PROBE_JTAG_IN_BASE);
    pio_sm_set_out_pins(pio0, PROBE_SM, PROBE_JTAG_OUT_BASE, 1);
    pio_sm_set_set_pins(pio0, PROBE_SM, PROBE_JTAG_SET_BASE, 1);
    PROBE_PIN_OUTPUT(PROBE_JTAG_OUT_BASE);
    PROBE_PIN_OUTPUT(PROBE_JTAG_SET_BASE);
    PROBE_PIN_INPUT(PROBE_JTAG_IN_BASE);
}

static void probe_set_swd_pins(void){
    pio_sm_set_in_pins(pio0, PROBE_SM, PROBE_SWD_DI_PIN);
    pio_sm_set_out_pins(pio0, PROBE_SM, PROBE_SWD_DO_PIN, 1);
    pio_sm_set_set_pins(pio0, PROBE_SM, PROBE_SWD_DO_PIN, 1);
    PROBE_PIN_INPUT(PROBE_JTAG_TDI_PIN);
    PROBE_PIN_INPUT(PROBE_JTAG_TDO_PIN);
    PROBE_PIN_INPUT(PROBE_SWD_DI_PIN);
    PROBE_PIN_OUTPUT(PROBE_SWD_CLK_PIN);
    PROBE_PIN_OUTPUT(PROBE_SWD_DOE_PIN);
    PROBE_PIN_OUTPUT(PROBE_SWD_DO_PIN);
}

void probe_enter_jtag(void){
    if(probe.mode == PROBE_MODE_JTAG){
        /* Already bound to JTAG, do not change the pin directions again */
        return;
    }

    probe_wait_idle();

    /* Pin binding and directions must be changed while the SM is stopped:
     * pio_sm_set_pindirs_with_mask() works by injecting set pindirs, whose
     * execution time is unpredictable while the SM runs */
    probe_sm_stop();

    /* GPIO0 must go to the PIO for TDI, so move the CDC bridge TX to GPIO12 first */
    cdc_uart_set_tx_pin(PROBE_UART_TX_JTAG_PIN);

    pio_gpio_init(pio0, PROBE_JTAG_TDI_PIN);
    gpio_disable_pulls(PROBE_JTAG_TDI_PIN);

    probe_set_jtag_pins();
    probe_sm_start();
    jtag_tms_valid = false;
    probe.mode = PROBE_MODE_JTAG;
}

void probe_enter_swd(void){
    if(probe.mode == PROBE_MODE_SWD){
        return;
    }

    probe_wait_idle();

    probe_sm_stop();

    /* TDI is unused in SWD mode, GPIO0 returns to the CDC bridge as TX */
    cdc_uart_set_tx_pin(PROBE_UART_TX_SWD_PIN);

    probe_set_swd_pins();
    probe_sm_start();
    jtag_tms_valid = false;
    probe.mode = PROBE_MODE_SWD;
}

void probe_assert_reset(bool state)
{
#if defined(PROBE_RESET_PIN)
    /* Change the direction to out to drive pin to 0 or to in to emulate open drain */
    gpio_set_dir(PROBE_RESET_PIN, state == 0 ? GPIO_OUT : GPIO_IN);
#endif
}

int probe_reset_level(void)
{
#if defined(PROBE_RESET_PIN)
    return gpio_get(PROBE_RESET_PIN);
#else
    return 0;
#endif
}

typedef enum probe_pio_command {
    CMD_WRITE = 0,          /* Max bit count is 128 */
    CMD_SHORT_WRITE,        /* Max bit count is 20 */
    CMD_SKIP,
    CMD_TURNAROUND,         /* Max bit count is 128 */
    CMD_READ,               /* Max bit count is 128, only the last 32 bits are kept beyond 32 bits */
    CMD_JTAG_TMS_LO,        /* No clock, TMS low */
    CMD_JTAG_TMS_HI,        /* No clock, TMS high */
    CMD_JTAG_TMS_SEQ_LO,    /* TMS low + 1 clock, then jump to the next field */
    CMD_JTAG_TMS_SEQ_HI,    /* TMS high + 1 clock, then jump to the next field */
    CMD_JTAG_RW,            /* Max bit count is 32 */
    CMD_JTAG_RW_SHORT,      /* Max bit count is 20 */
} probe_pio_command_t;

/* Command word layout: bit count [6:0] (value = bits - 1), command entry address [11:7],
 * short command inline data [31:12] */
#define PROBE_CMD_COUNT_MASK (PROBE_CMD_MAX_BITS - 1u)
#define PROBE_CMD_ADDR_SHIFT 7
#define PROBE_CMD_ADDR_WIDTH 5
#define PROBE_CMD_DATA_SHIFT 12
#define PROBE_SHORT_MAX_BITS 20u    /* Max bits of inline data in a short command */

static inline uint32_t fmt_probe_command(uint bit_count, probe_pio_command_t cmd) {
    const uint cmd_offset[] = {
        probe_offset_write_cmd,
        probe_offset_short_write_cmd,
        probe_offset_get_next_cmd,
        probe_offset_turnaround_cmd,
        probe_offset_read_cmd,
        probe_offset_jtag_tms_lo_cmd,
        probe_offset_jtag_tms_hi_cmd,
        probe_offset_jtag_tms_seq_lo_cmd,
        probe_offset_jtag_tms_seq_hi_cmd,
        probe_offset_jtag_rw_cmd,
        probe_offset_jtag_rw_short_cmd,
    };
    /* out pc, 5 writes an absolute program address, so the entry address must include the
     * program load offset; the SDK relocates jmp at load time, sharing probe.offset matches both */
    uint cmd_addr = cmd_offset[cmd] + probe.offset;
    return ((bit_count - 1) & PROBE_CMD_COUNT_MASK) | (cmd_addr << PROBE_CMD_ADDR_SHIFT);
}

/* Entry with the largest relative address, used to validate the load offset */
static uint probe_max_cmd_offset(void) {
    const uint cmd_offset[] = {
        probe_offset_write_cmd,
        probe_offset_short_write_cmd,
        probe_offset_get_next_cmd,
        probe_offset_turnaround_cmd,
        probe_offset_read_cmd,
        probe_offset_jtag_tms_lo_cmd,
        probe_offset_jtag_tms_hi_cmd,
        probe_offset_jtag_tms_seq_lo_cmd,
        probe_offset_jtag_tms_seq_hi_cmd,
        probe_offset_jtag_rw_cmd,
        probe_offset_jtag_rw_short_cmd,
    };
    uint max = 0;

    for(uint i = 0; i < sizeof(cmd_offset) / sizeof(cmd_offset[0]); i++){
        if(cmd_offset[i] > max){
            max = cmd_offset[i];
        }
    }
    return max;
}

void probe_write_short_bits(uint bit_count, uint32_t data_byte) {
    probe_put(fmt_probe_command(bit_count, CMD_SHORT_WRITE) | (data_byte << PROBE_CMD_DATA_SHIFT));
}

void probe_write_bits(uint bit_count, uint32_t data_byte) {
    /* The data word is only 32 bits wide, bits beyond 32 are driven as 0; the count field is
     * 7 bits wide, callers keep the bit width within 128 */
    if(bit_count <= PROBE_SHORT_MAX_BITS){
        probe_write_short_bits(bit_count, data_byte);
    }else{
        probe_put(fmt_probe_command(bit_count, CMD_WRITE));
        probe_put(data_byte);
    }
    // Return immediately so we can cue up the next command whilst this one runs
}

void probe_hiz_clocks(uint bit_count) {
    while(bit_count > PROBE_SHORT_MAX_BITS){
        probe_put(fmt_probe_command(PROBE_SHORT_MAX_BITS, CMD_TURNAROUND));
        bit_count -= PROBE_SHORT_MAX_BITS;
    }
    probe_put(fmt_probe_command(bit_count, CMD_TURNAROUND));
}

uint32_t probe_read_bits(uint bit_count) {
    probe_put(fmt_probe_command(bit_count, CMD_READ));
    uint32_t data = probe_get();
    uint32_t data_shifted = data;
    if (bit_count < 32) {
        data_shifted = data >> (32 - bit_count);
    }

    return data_shifted;
}

void jtag_tms(uint32_t tms) {
    const uint8_t level = tms ? 1u : 0u;

    if(jtag_tms_valid && (jtag_tms_level == level)){
        return;
    }
    probe_put(fmt_probe_command(1, level ? CMD_JTAG_TMS_HI : CMD_JTAG_TMS_LO));
    jtag_tms_level = level;
    jtag_tms_valid = true;
}

void jtag_tms_seq(uint32_t n, uint32_t tms) {
    const uint32_t lo = probe.offset + probe_offset_jtag_tms_seq_lo_cmd;
    const uint32_t hi = probe.offset + probe_offset_jtag_tms_seq_hi_cmd;
    uint32_t word;
    uint32_t i;

    if((n == 0u) || (n > PROBE_TMS_SEQ_MAX)){
        return;
    }

    /* Field n holds the address returning to get_next_cmd, the first n fields are the per-cycle
     * entries; higher fields are never read */
    word = (probe.offset + probe_offset_get_next_cmd) << (PROBE_CMD_ADDR_SHIFT + (PROBE_CMD_ADDR_WIDTH * n));
    for(i = 0; i < n; i++){
        word |= (((tms >> i) & 1u) ? hi : lo) << (PROBE_CMD_ADDR_SHIFT + (PROBE_CMD_ADDR_WIDTH * i));
    }
    probe_put(word);

    /* The TMS level of the last cycle is the level after the sequence ends */
    jtag_tms_level = (uint8_t)((tms >> (n - 1u)) & 1u);
    jtag_tms_valid = true;
}

uint32_t jtag_bits(uint32_t n, bool tms, uint32_t tdi, bool capture) {
    if((n == 0u) || (n > 32u)){
        /* Callers are responsible for chunking */
        return 0u;
    }

    jtag_tms(tms);

    if(!capture){
        /* Without capture the CMD_WRITE / CMD_SHORT_WRITE path must be taken: the JTAG command
         * pushes at the end of its bit loop, and an unread value fills the RX FIFO (4 words);
         * the SM then stalls on push and stops consuming commands, after which probe_put()
         * waits forever for TX FIFO space. Both commands use the same side sequence and bit
         * loop as the capture path (out pins,1 [1] side 0x2 / jmp x-- [1] side 0x3), so the
         * waveform is unaffected */
        probe_write_bits(n, tdi);
        return 0u;
    }

    if(n <= PROBE_SHORT_MAX_BITS){
        /* The short command inlines TDI in the command word, so one FIFO write suffices.
         * The shift width matches the SWD short command (<<12): the OSR shifts right and
         * out x,7 plus out pc,5 already consumed 12 bits before the bit loop, so the data
         * must sit at [31:12] for the lowest bit to land at the first position read */
        const uint32_t mask = (1u << n) - 1u;
        probe_put(fmt_probe_command(n, CMD_JTAG_RW_SHORT) | ((tdi & mask) << PROBE_CMD_DATA_SHIFT));
    }else{
        probe_put(fmt_probe_command(n, CMD_JTAG_RW));
        probe_put(tdi);
    }

    const uint32_t raw = probe_get();
    const uint32_t shifted = (n < 32u) ? (raw >> (32u - n)) : raw;
    return shifted;
}

/* Whether the SM has drained its commands and is parked at the dispatch point */
bool probe_is_idle(void) {
    const uint32_t dispatch = (uint32_t)(probe.offset + probe_offset_get_next_cmd) & PIO_SM0_ADDR_BITS;

    return (pio0->fstat & PIO_FSTAT_TXEMPTY_BITS) &&
           ((pio0->sm[PROBE_SM].addr & PIO_SM0_ADDR_BITS) == dispatch);
}

void probe_wait_idle(void) {
    if(probe.fault){
        /* The fault already parked the SM, nothing is left to drain */
        return;
    }

    /* Use "TX FIFO empty plus PC at the dispatch point". FDEBUG TXSTALL is not used:
     * it is only set under some conditions, so comparing it can wait forever when the SM has
     * already stopped but no new stall event arrives */
    if(!probe_is_idle()){
        const uint64_t deadline = probe_deadline();

        while(!probe_is_idle()){
            if(time_us_64() >= deadline){
                /* Normally satisfied within the current command. Reaching here means the PIO
                 * state is abnormal: park the SM so the pin binding can still be changed, and
                 * report the fault instead of spinning */
                probe_fault_set();
                return;
            }
        }
    }
}

bool probe_take_timeout(void) {
    const bool fault = probe.fault;

    probe.fault = false;
    return fault;
}

/* SWD read/write direction is driven by the side-set (DOE) of the PIO commands, this empty implementation keeps
 * the old interface */
void probe_read_mode(void) {
}

void probe_write_mode(void) {
}

void probe_init() {
    if (!probe.initted) {
        probe_gpio_init(pio0);

        /* The offset may be any valid value: the entry address in the command word includes
         * probe.offset, matching the jmp relocation the SDK performs at load time. Any
         * non-negative load offset works as long as offset plus the largest entry fits the
         * 5-bit PC address range */
        int offset = pio_add_program(pio0, &probe_program);
        hard_assert(offset >= 0);
        hard_assert((uint)offset + probe_max_cmd_offset() < 32);
        probe.offset = (uint)offset;

        pio_sm_config sm_config = probe_program_get_default_config(probe.offset);
        probe_sm_init(pio0, &sm_config);
        pio_sm_init(pio0, PROBE_SM, probe.offset, &sm_config);

        /* pio_sm_init() loads the default divider, so re-apply the last requested frequency */
        probe_set_swclk_freq_hz(swclk_requested_hz);

        /* Bind the SWD pins before starting the SM. In the opposite order the SM parks on pull
         * right after start, and probe_wait_idle() would wait for a stall event that may never
         * come again */
        probe.mode = PROBE_MODE_NONE;
        jtag_tms_valid = false;
        probe_set_swd_pins();

        // Jump SM to command dispatch routine, and enable it
        pio_sm_exec(pio0, PROBE_SM, probe.offset + probe_offset_get_next_cmd);
        pio_sm_set_enabled(pio0, PROBE_SM, 1);
        probe.mode = PROBE_MODE_SWD;
        probe.initted = 1;
    }
}

void probe_deinit(void)
{
#if 0
  if (probe.initted) {
    probe_read_mode();
    pio_sm_set_enabled(pio0, PROBE_SM, 0);
    pio_remove_program(pio0, &probe_program, probe.offset);

    probe_assert_reset(1);	// de-assert nRESET
    probe_gpio_deinit();
    probe.initted = 0;
  }
#endif
}
