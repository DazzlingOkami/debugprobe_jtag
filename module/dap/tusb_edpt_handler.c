/**
 * Copyright (c) 2023 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hardware/watchdog.h"
#include "tusb_edpt_handler.h"
#include "DAP.h"
#include "dap_stats.h"
#include "semphr.h"

static uint8_t itf_num;
static uint8_t _rhport;

static uint8_t _out_ep_addr;
static uint8_t _in_ep_addr;

static buffer_t USBRequestBuffer;
static buffer_t USBResponseBuffer;

/* Serializes ring index updates and endpoint arming against the USB callbacks */
static SemaphoreHandle_t edpt_mutex;

#if DAP_STATS_ENABLE
/* Throughput counters, all updates are done with edpt_mutex held */
static dap_stats_t edpt_stats;

/* Timestamp of the previous OUT completion, used for the gap statistics */
static uint32_t last_out_us;
static bool out_gap_valid;
#endif

#define WR_IDX(x) (x.wptr % DAP_BUFFER_COUNT)
#define RD_IDX(x) (x.rptr % DAP_BUFFER_COUNT)

#define WR_SLOT_PTR(x) &(x.data[WR_IDX(x)][0])
#define RD_SLOT_PTR(x) &(x.data[RD_IDX(x)][0])

/* wptr/rptr are free-running counters, so compare slot indices instead of raw
 * values. 2^32 is a multiple of DAP_BUFFER_COUNT, which keeps the index valid
 * across counter wrap. */
bool buffer_full(buffer_t *buffer)
{
	return (((buffer->wptr + 1U) % DAP_BUFFER_COUNT) == (buffer->rptr % DAP_BUFFER_COUNT));
}

bool buffer_empty(buffer_t *buffer)
{
	return (buffer->wptr == buffer->rptr);
}

void dap_edpt_init(void)
{
	edpt_mutex = xSemaphoreCreateMutex();
}

bool dap_edpt_deinit(void)
{
	memset(&USBRequestBuffer, 0, sizeof(USBRequestBuffer));
	memset(&USBResponseBuffer, 0, sizeof(USBResponseBuffer));
	if (edpt_mutex != NULL) {
		vSemaphoreDelete(edpt_mutex);
		edpt_mutex = NULL;
	}
	return true;
}

#if DAP_STATS_ENABLE
/* Track the deepest ring occupancy seen, called with edpt_mutex held */
static void stats_note_depth(uint8_t *max_depth, uint32_t depth)
{
	if (depth > *max_depth) {
		*max_depth = (uint8_t) depth;
	}
}

void dap_stats_get(dap_stats_t *stats)
{
	xSemaphoreTake(edpt_mutex, portMAX_DELAY);

	*stats = edpt_stats;
	stats->uptime_ms = (uint32_t) xTaskGetTickCount();
	stats->swclk_khz = (uint32_t) probe_get_swclk_freq();
	stats->req_depth = (uint8_t) (USBRequestBuffer.wptr - USBRequestBuffer.rptr);
	stats->resp_depth = (uint8_t) (USBResponseBuffer.wptr - USBResponseBuffer.rptr);

	xSemaphoreGive(edpt_mutex);
}

void dap_stats_reset(void)
{
	if (edpt_mutex == NULL) {
		memset(&edpt_stats, 0, sizeof(edpt_stats));
		out_gap_valid = false;
		return;
	}

	xSemaphoreTake(edpt_mutex, portMAX_DELAY);
	memset(&edpt_stats, 0, sizeof(edpt_stats));
	out_gap_valid = false;
	xSemaphoreGive(edpt_mutex);
}
#endif

void dap_edpt_reset(uint8_t __unused rhport)
{
	itf_num = 0;
}

char * dap_cmd_string[] = {
	[ID_DAP_Info               ] = "DAP_Info",
	[ID_DAP_HostStatus         ] = "DAP_HostStatus",
	[ID_DAP_Connect            ] = "DAP_Connect",
	[ID_DAP_Disconnect         ] = "DAP_Disconnect",
	[ID_DAP_TransferConfigure  ] = "DAP_TransferConfigure",
	[ID_DAP_Transfer           ] = "DAP_Transfer",
	[ID_DAP_TransferBlock      ] = "DAP_TransferBlock",
	[ID_DAP_TransferAbort      ] = "DAP_TransferAbort",
	[ID_DAP_WriteABORT         ] = "DAP_WriteABORT",
	[ID_DAP_Delay              ] = "DAP_Delay",
	[ID_DAP_ResetTarget        ] = "DAP_ResetTarget",
	[ID_DAP_SWJ_Pins           ] = "DAP_SWJ_Pins",
	[ID_DAP_SWJ_Clock          ] = "DAP_SWJ_Clock",
	[ID_DAP_SWJ_Sequence       ] = "DAP_SWJ_Sequence",
	[ID_DAP_SWD_Configure      ] = "DAP_SWD_Configure",
	[ID_DAP_SWD_Sequence       ] = "DAP_SWD_Sequence",
	[ID_DAP_JTAG_Sequence      ] = "DAP_JTAG_Sequence",
	[ID_DAP_JTAG_Configure     ] = "DAP_JTAG_Configure",
	[ID_DAP_JTAG_IDCODE        ] = "DAP_JTAG_IDCODE",
	[ID_DAP_SWO_Transport      ] = "DAP_SWO_Transport",
	[ID_DAP_SWO_Mode           ] = "DAP_SWO_Mode",
	[ID_DAP_SWO_Baudrate       ] = "DAP_SWO_Baudrate",
	[ID_DAP_SWO_Control        ] = "DAP_SWO_Control",
	[ID_DAP_SWO_Status         ] = "DAP_SWO_Status",
	[ID_DAP_SWO_ExtendedStatus ] = "DAP_SWO_ExtendedStatus",
	[ID_DAP_SWO_Data           ] = "DAP_SWO_Data",
	[ID_DAP_QueueCommands      ] = "DAP_QueueCommands",
	[ID_DAP_ExecuteCommands    ] = "DAP_ExecuteCommands",
};

/* Map a command ID to a printable name without indexing past the table */
static const char * dap_cmd_name(uint8_t cmd) __attribute__((unused));
static const char * dap_cmd_name(uint8_t cmd)
{
	if (cmd >= ID_DAP_Vendor0 && cmd <= ID_DAP_Vendor31) {
		return "DAP_Vendor";
	}
	if (cmd >= (sizeof(dap_cmd_string) / sizeof(dap_cmd_string[0])) || dap_cmd_string[cmd] == NULL) {
		return "DAP_Invalid";
	}
	return dap_cmd_string[cmd];
}


uint16_t dap_edpt_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{

	TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == itf_desc->bInterfaceClass &&
			DAP_INTERFACE_SUBCLASS == itf_desc->bInterfaceSubClass &&
			DAP_INTERFACE_PROTOCOL == itf_desc->bInterfaceProtocol, 0);

	/* Remember the port number; dap_thread needs it when queueing endpoint transfers */
	_rhport = rhport;

	//  Initialise circular buffer indices
	USBResponseBuffer.wptr = 0;
	USBResponseBuffer.rptr = 0;
	USBRequestBuffer.wptr = 0;
	USBRequestBuffer.rptr = 0;

	// Initialse full/empty flags
	USBResponseBuffer.wasFull = false;
	USBRequestBuffer.wasFull = false;

#if DAP_STATS_ENABLE
	// Throughput counters start from zero for every new connection
	dap_stats_reset();
#endif

	uint16_t const drv_len = sizeof(tusb_desc_interface_t) + (itf_desc->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
	TU_VERIFY(max_len >= drv_len, 0);
	itf_num = itf_desc->bInterfaceNumber;

	// Initialising the OUT endpoint

	tusb_desc_endpoint_t *edpt_desc = (tusb_desc_endpoint_t *) (itf_desc + 1);
	uint8_t ep_addr = edpt_desc->bEndpointAddress;

	_out_ep_addr = ep_addr;

	/* The OUT endpoint requires a call to usbd_edpt_xfer to initialise the endpoint, giving tinyUSB a buffer
	 * to consume when a transfer occurs at the endpoint */
	usbd_edpt_open(rhport, edpt_desc);
	usbd_edpt_xfer(rhport, ep_addr, WR_SLOT_PTR(USBRequestBuffer), DAP_PACKET_SIZE);

	// Initiliasing the IN endpoint

	edpt_desc++;
	ep_addr = edpt_desc->bEndpointAddress;

	_in_ep_addr = ep_addr;

	// The IN endpoint doesn't need a transfer to initialise it, as this will be done by the main loop of dap_thread
	usbd_edpt_open(rhport, edpt_desc);

	return drv_len;

}

bool dap_edpt_control_xfer_cb(uint8_t __unused rhport, uint8_t stage, tusb_control_request_t const *request)
{
	return false;
}

// Manage USBResponseBuffer (response) write and USBRequestBuffer (request) read indices
bool dap_edpt_xfer_cb(uint8_t __unused rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
	const uint8_t ep_dir = tu_edpt_dir(ep_addr);
#if DAP_STATS_ENABLE
	uint32_t req_depth;
	uint32_t now_us;
	uint32_t gap_us;
#endif

	(void) result;

	if (xferred_bytes > DAP_PACKET_SIZE)
	{
		return false;
	}

	xSemaphoreTake(edpt_mutex, portMAX_DELAY);

	if (ep_dir == TUSB_DIR_IN)
	{
		USBResponseBuffer.rptr++;
#if DAP_STATS_ENABLE
		edpt_stats.in_packets++;
#endif

		/* Chain the next queued response; the DAP thread only arms the first one */
		if (USBResponseBuffer.rptr != USBResponseBuffer.wptr)
		{
			usbd_edpt_xfer(rhport, ep_addr, RD_SLOT_PTR(USBResponseBuffer),
				       USBResponseBuffer.data_len[RD_IDX(USBResponseBuffer)]);
		}
	}
	else if (ep_dir == TUSB_DIR_OUT)
	{
		/* Only queue the next buffer if the ring is not full; otherwise the DAP
		 * thread re-arms the endpoint once it has consumed a packet */
		if (!buffer_full(&USBRequestBuffer))
		{
			USBRequestBuffer.wptr++;
			usbd_edpt_xfer(rhport, ep_addr, WR_SLOT_PTR(USBRequestBuffer), DAP_PACKET_SIZE);
			USBRequestBuffer.wasFull = false;
		}
		else
		{
			USBRequestBuffer.wasFull = true;
		}

#if DAP_STATS_ENABLE
		/* Record how many requests the host had queued at once */
		req_depth = USBRequestBuffer.wptr - USBRequestBuffer.rptr;
		if (USBRequestBuffer.wasFull)
		{
			/* The completed packet is in the ring but not counted yet */
			req_depth++;
		}

		edpt_stats.out_packets++;
		stats_note_depth(&edpt_stats.max_req_depth, req_depth);

		/* Time between consecutive requests shows how the host paces packets */
		now_us = time_us_32();
		if (out_gap_valid)
		{
			gap_us = now_us - last_out_us;
			if ((edpt_stats.min_out_gap_us == 0) || (gap_us < edpt_stats.min_out_gap_us))
			{
				edpt_stats.min_out_gap_us = gap_us;
			}
			if (gap_us > edpt_stats.max_out_gap_us)
			{
				edpt_stats.max_out_gap_us = gap_us;
			}
		}
		last_out_us = now_us;
		out_gap_valid = true;
#endif
	}
	else
	{
		xSemaphoreGive(edpt_mutex);
		return false;
	}

	xSemaphoreGive(edpt_mutex);

	/* Wake the DAP thread, there is either a new request or a free response slot */
	if (dap_taskhandle != NULL)
	{
		xTaskNotify(dap_taskhandle, 0, eNoAction);
	}

	return true;
}

/* Feed the watchdog from this thread: a dense request stream keeps it running
 * on core 1 and starves the low priority watchdog task there. */
#define DAP_WATCHDOG_FEED_MS 500U

static void dap_watchdog_feed(void)
{
	static TickType_t last_feed;
	TickType_t now = xTaskGetTickCount();

	if ((now - last_feed) >= pdMS_TO_TICKS(DAP_WATCHDOG_FEED_MS)) {
		last_feed = now;
		watchdog_update();
	}
}

void dap_thread(void *ptr)
{
	uint32_t n;
	uint32_t notified;
#if DAP_STATS_ENABLE
	uint32_t start_us;
	uint32_t busy_us;
#endif
	uint16_t resp_len;
	bool was_empty;

#if (configNUMBER_OF_CORES > 1)
    vTaskCoreAffinitySet(NULL, 1 << 1);
#endif

	for (;;)
	{
		/* Sleep until a USB callback queued a request or freed a response slot */
		xTaskNotifyWait(0, 0xFFFFFFFFu, &notified, 1);

		while (USBRequestBuffer.rptr != USBRequestBuffer.wptr)
		{
			dap_watchdog_feed();

			/*
			 * Atomic command support - buffer QueueCommands, but don't process them
			 * until a non-QueueCommands packet is seen.
			 */
			n = USBRequestBuffer.rptr;
			while (USBRequestBuffer.data[n % DAP_BUFFER_COUNT][0] == ID_DAP_QueueCommands) {
				probe_info("%lu %lu DAP queued cmd %s len %02x\n",
					       USBRequestBuffer.wptr, USBRequestBuffer.rptr,
					       dap_cmd_name(USBRequestBuffer.data[n % DAP_BUFFER_COUNT][0]),
					       USBRequestBuffer.data[n % DAP_BUFFER_COUNT][1]);
				USBRequestBuffer.data[n % DAP_BUFFER_COUNT][0] = ID_DAP_ExecuteCommands;
				n++;
				while (n == USBRequestBuffer.wptr) {
					/* The rest of the batch has not arrived yet */
					xTaskNotifyWait(0, 0xFFFFFFFFu, &notified, 1);
				}
			}

			/* Never overwrite a response the host has not read yet */
			while (buffer_full(&USBResponseBuffer)) {
				xTaskNotifyWait(0, 0xFFFFFFFFu, &notified, 1);
			}

			/* Process the request in place, the response goes to its own ring slot */
#if DAP_STATS_ENABLE
			start_us = time_us_32();
#endif
			resp_len = (uint16_t) DAP_ExecuteCommand(RD_SLOT_PTR(USBRequestBuffer),
								 WR_SLOT_PTR(USBResponseBuffer));
#if DAP_STATS_ENABLE
			busy_us = time_us_32() - start_us;
#endif
			probe_info("%lu %lu DAP resp %s len %u\n",
				       USBResponseBuffer.wptr, USBResponseBuffer.rptr,
				       dap_cmd_name(WR_SLOT_PTR(USBResponseBuffer)[0]), resp_len);

			xSemaphoreTake(edpt_mutex, portMAX_DELAY);

			was_empty = buffer_empty(&USBResponseBuffer);

			USBResponseBuffer.data_len[WR_IDX(USBResponseBuffer)] = resp_len;
			USBResponseBuffer.wptr++;
			USBRequestBuffer.rptr++;

#if DAP_STATS_ENABLE
			/* Track how much time the device spends inside the command */
			edpt_stats.busy_us += busy_us;
			if (busy_us > edpt_stats.max_busy_us)
			{
				edpt_stats.max_busy_us = busy_us;
			}

			/* Track the deepest response backlog (queued plus in flight) */
			stats_note_depth(&edpt_stats.max_resp_depth,
					 USBResponseBuffer.wptr - USBResponseBuffer.rptr);
#endif

			/* The OUT callback stops re-arming when the ring is full, resume here */
			if (USBRequestBuffer.wasFull)
			{
				USBRequestBuffer.wptr++;
				usbd_edpt_xfer(_rhport, _out_ep_addr, WR_SLOT_PTR(USBRequestBuffer), DAP_PACKET_SIZE);
				USBRequestBuffer.wasFull = false;
			}

			if (was_empty)
			{
				usbd_edpt_xfer(_rhport, _in_ep_addr, RD_SLOT_PTR(USBResponseBuffer),
					       USBResponseBuffer.data_len[RD_IDX(USBResponseBuffer)]);
			}

			xSemaphoreGive(edpt_mutex);
		}
	}
}

usbd_class_driver_t const _dap_edpt_driver =
{
		.init = dap_edpt_init,
		.deinit = dap_edpt_deinit,
		.reset = dap_edpt_reset,
		.open = dap_edpt_open,
		.control_xfer_cb = dap_edpt_control_xfer_cb,
		.xfer_cb = dap_edpt_xfer_cb,
		.sof = NULL,
#if CFG_TUSB_DEBUG >= 2
		.name = "DAP ENDPOINT"
#endif
};

// Add the custom driver to the tinyUSB stack
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
	*driver_count = 1;
	return &_dap_edpt_driver;
}

