#include "hardware/structs/usb.h"
#include "tusb.h"
#include "FreeRTOS.h"
#include "task.h"
#include "dap/tusb_edpt_handler.h"
#include "DAP.h"

#define TUD_TASK_PRIO 2
#define DAP_TASK_PRIO 3

static TaskHandle_t usb_taskhandle;

TaskHandle_t dap_taskhandle;

/* TinyUSB queues an event from the USB interrupt; wake usb_thread right away so a
 * transfer callback is not delayed until the next tick */
void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    (void) rhport;
    (void) eventid;

    if (usb_taskhandle == NULL) {
        return;
    }

    if (in_isr) {
        BaseType_t woken = pdFALSE;
        xTaskNotifyFromISR(usb_taskhandle, 0, eNoAction, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        xTaskNotify(usb_taskhandle, 0, eNoAction);
    }
}

static void usb_thread(void *p)
{
    uint32_t notified;

#if (configNUMBER_OF_CORES > 1)
    vTaskCoreAffinitySet(NULL, 1 << 0);
#endif

#ifdef PROBE_USB_CONNECTED_LED
    gpio_init(PROBE_USB_CONNECTED_LED);
    gpio_set_dir(PROBE_USB_CONNECTED_LED, GPIO_OUT);
#endif

    usb_taskhandle = xTaskGetCurrentTaskHandle();

    TickType_t wake;
    wake = xTaskGetTickCount();
    do {
        tud_task();
#ifdef PROBE_USB_CONNECTED_LED
        if (!gpio_get(PROBE_USB_CONNECTED_LED) && tud_ready())
            gpio_put(PROBE_USB_CONNECTED_LED, 1);
        else
            gpio_put(PROBE_USB_CONNECTED_LED, 0);
#endif
        // If suspended or disconnected, delay for 1ms (20 ticks)
        if (tud_suspended() || !tud_connected())
            xTaskDelayUntil(&wake, pdMS_TO_TICKS(1));
        // Go to sleep until TinyUSB queues another event
        else if (!tud_task_event_ready())
            xTaskNotifyWait(0, 0xFFFFFFFFu, &notified, 1);
    } while (1);
}

int usb_init(void){
#if defined(CFG_TUD_MSC_ASYNC_IO) && (CFG_TUD_MSC_ASYNC_IO)
    void msc_task_init(void);
    msc_task_init();
#endif

    xTaskCreate(usb_thread, "USB", configMINIMAL_STACK_SIZE, NULL, TUD_TASK_PRIO, NULL);

    /* The DAP thread lives for the whole session; mount and unmount only reset
     * the endpoint rings, so there is nothing to create or delete per mount. */
    xTaskCreate(dap_thread, "DAP", configMINIMAL_STACK_SIZE, NULL, DAP_TASK_PRIO, &dap_taskhandle);

    return 0;
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void) remote_wakeup_en;
    probe_info("Suspended\n");
    /* slow down clk_sys for power saving ? */
}

void tud_resume_cb(void)
{
    probe_info("Resumed\n");
}

void tud_unmount_cb(void)
{
    probe_info("Disconnected\n");
}

void tud_mount_cb(void)
{
    probe_info("Connected, Configured\n");
}


uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    // TODO not Implemented
    (void) itf;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;

    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* RxDataBuffer, uint16_t bufsize)
{
    uint32_t response_size = TU_MIN(CFG_TUD_HID_EP_BUFSIZE, bufsize);

    // This doesn't use multiple report and report ID
    (void) itf;
    (void) report_id;
    (void) report_type;

    static uint8_t TxDataBuffer[CFG_TUD_HID_EP_BUFSIZE];
    DAP_ProcessCommand(RxDataBuffer, TxDataBuffer);

    tud_hid_report(0, TxDataBuffer, response_size);
}

#if (PROBE_DEBUG_PROTOCOL == PROTO_DAP_V2)
extern uint8_t const desc_ms_os_20[];

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request)
{
    // nothing to with DATA & ACK stage
    if (stage != CONTROL_STAGE_SETUP) return true;

    switch (request->bmRequestType_bit.type)
    {
        case TUSB_REQ_TYPE_VENDOR:
        switch (request->bRequest)
        {
            case 1:
            if ( request->wIndex == 7 )
            {
                // Get Microsoft OS 2.0 compatible descriptor
                uint16_t total_len;
                memcpy(&total_len, desc_ms_os_20+8, 2);

                return tud_control_xfer(rhport, request, (void*) desc_ms_os_20, total_len);
            }else
            {
                return false;
            }

            default: break;
        }
        break;
        default: break;
    }

    // stall unknown request
    return false;
}
#endif

