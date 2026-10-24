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

#ifndef CDC_UART_H
#define CDC_UART_H

void cdc_uart_init(void);
void cdc_thread_create(void);

int cdc_get_baudrate(void);
void cdc_set_baudrate(int baudrate);

/**
 * @brief Switch UART TX to the given pin
 * @param[in] pin Target TX pin, supports PROBE_UART_TX_SWD_PIN and PROBE_UART_TX_JTAG_PIN
 * @note Rebuilds the UART when the pin changes: abort the transfer, disable the UART, clear the buffers,
 *       reconfigure the pin, then reinitialize the UART and restart DMA and interrupts
 */
void cdc_uart_set_tx_pin(uint pin);

#endif