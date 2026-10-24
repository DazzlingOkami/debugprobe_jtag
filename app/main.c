/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2021 Peter Lawrence
 * Copyright (C) 2024 WangGaoJie <kinghd1912@163.com>
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

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bsp/board.h"
#include "tusb.h"
#include "usb/usb.h"
#include "swd/probe_config.h"
#include "swd/probe.h"
#include "uart/cdc_uart.h"
#include "iic_shard/iic_shard.h"
#include "dac_mcp4716/mcp4716.h"
#include "utils/get_serial.h"
#include "DAP.h"
#include "log/log.h"
#include "dap_board.h"

#define BMP_TASK_PRIO  (tskIDLE_PRIORITY + 3)

static void watchdog_task(void *ptr){
    if (watchdog_caused_reboot()){
        LOG("watch dog timeout. reset boot.");
        reset_usb_boot(0, 0);
    }

    watchdog_enable(1000, 1);
    while(1){
#if (configNUMBER_OF_CORES > 1)
        /* Update watchdog on all cores */
        for(int i = 0; i < configNUMBER_OF_CORES; i++){
            vTaskCoreAffinitySet(NULL, 1 << i);
            watchdog_update();
            vTaskDelay(pdMS_TO_TICKS(600));
        }
#else
        watchdog_update();
        vTaskDelay(pdMS_TO_TICKS(600));
#endif
    }
}

void start_task(void *ptr){
#if (configNUMBER_OF_CORES > 1)
    vTaskCoreAffinitySet(NULL, 1 << 0);
#endif
    xTaskCreate(watchdog_task, "WDOG", 512, NULL, 1, NULL);

    usb_init();

    cdc_thread_create();

    iic_shard_init();

    if(dac_mcp4716_init() != 0){
        LOG("dac mcp4716 init failed\n");
    }

    vTaskDelete(NULL);

    /* vTaskDelete(NULL) does not normally return; if it does, do not spin at priority 4 */
    while(1){
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void) {
    // Declare pins in binary information
    bi_decl_config();
    setvbuf(stdout, NULL, 0, _IONBF);
    board_init();
    usb_serial_init();

    cdc_uart_init();
    tusb_init();
    stdio_uart_init();
    DAP_Setup();

    probe_info("Welcome to debugprobe!\n");
    xTaskCreate(start_task, "start", 1024, NULL, 4, NULL);

    vTaskStartScheduler();

    return 0;
}

void vApplicationTickHook (void)
{
}

void vApplicationStackOverflowHook(TaskHandle_t Task, char *pcTaskName)
{
    panic("stack overflow (not the helpful kind) for %s\n", *pcTaskName);
}

void vApplicationMallocFailedHook(void)
{
    panic("Malloc Failed\n");
}
