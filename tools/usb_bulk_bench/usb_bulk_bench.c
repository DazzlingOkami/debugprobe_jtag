/*
 * SPDX-License-Identifier: MIT
 */

/**
 * @file    usb_bulk_bench.c
 * @brief   Bulk endpoint throughput benchmark for the CMSIS-DAP v2 interface.
 *
 * The firmware vendor command 0x83 consumes one 64-byte request packet and
 * returns a short response without looking at the payload, so this tool
 * measures the USB stack and the endpoint ring instead of the debug protocol.
 * Three scheduling modes are provided:
 *
 *   serial - one request in flight, OUT then IN, every round trip
 *   batch  - a window of requests per round trip, like OpenOCD's block queue
 *   async  - continuous windows of OUT and IN transfers
 *
 * Every response echoes the request sequence number, so a lost or reordered
 * packet is reported instead of silently reducing the throughput.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "libusb.h"

#define BENCH_CMD               (0x83U)
#define DAP_PACKET_SIZE         (64)
#define BENCH_MIN_RESP          (4)
#define BENCH_MAX_RESP          (DAP_PACKET_SIZE)

#define DEFAULT_VID             (0x2E8AU)
#define DEFAULT_PID             (0x000CU)
#define DEFAULT_DEPTH           (8)
#define DEFAULT_RESP_LEN        (4)
#define DEFAULT_SECONDS         (5)
#define DEFAULT_WRITE_ADDR      (0x24000000U)
#define DEFAULT_WRITE_WORDS     (14)
#define DEFAULT_SWCLK_HZ        (20000000U)

#define TRANSFER_TIMEOUT_MS     (1000)
#define DRAIN_TIMEOUT_MS        (2000)

typedef enum {
    MODE_SERIAL,
    MODE_BATCH,
    MODE_ASYNC,
    MODE_DAP_WRITE
} bench_mode_t;

typedef struct {
    libusb_device_handle *dev;
    uint8_t ep_out;
    uint8_t ep_in;
    bench_mode_t mode;
    int depth;
    int resp_len;

    uint8_t *out_buf;
    uint8_t *in_buf;
    struct libusb_transfer **out_xfer;
    struct libusb_transfer **in_xfer;

    int out_done;
    int in_done;
    int out_inflight;
    int in_inflight;
    int refill;

    int write_mode;
    uint32_t write_addr;
    uint32_t swclk;
    int write_words;

    uint32_t seq_submit;
    uint32_t seq_check;
    uint64_t packets;
    uint64_t limit;
    uint64_t out_bytes;
    uint64_t in_bytes;
    uint64_t errors;
    int stop;
    int draining;
} bench_t;

typedef struct {
    bench_mode_t mode;
    int depth;
    int resp_len;
    int seconds;
    int interface;
    uint16_t vid;
    uint16_t pid;
    uint32_t write_addr;
    uint32_t swclk;
    int write_words;
    uint64_t limit;
    int list;
} options_t;

static uint64_t now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&counter);

    return (uint64_t)(counter.QuadPart * 1000000ULL / (uint64_t)freq.QuadPart);
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

static const char *mode_name(bench_mode_t mode)
{
    switch (mode) {
    case MODE_SERIAL:
        return "serial";
    case MODE_BATCH:
        return "batch";
    case MODE_DAP_WRITE:
        return "swd-write";
    default:
        return "async";
    }
}

static const char *xfer_status_name(enum libusb_transfer_status status)
{
    switch (status) {
    case LIBUSB_TRANSFER_COMPLETED:
        return "completed";
    case LIBUSB_TRANSFER_ERROR:
        return "error";
    case LIBUSB_TRANSFER_TIMED_OUT:
        return "timed out";
    case LIBUSB_TRANSFER_CANCELLED:
        return "cancelled";
    case LIBUSB_TRANSFER_STALL:
        return "stall";
    case LIBUSB_TRANSFER_NO_DEVICE:
        return "no device";
    case LIBUSB_TRANSFER_OVERFLOW:
        return "overflow";
    default:
        return "unknown";
    }
}

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/* Bench request layout: [cmd, seq_lo, seq_hi, resp_len, ignored payload].
 * SWD request layout: DAP_TransferBlock write of write_words words to DRW. */
static void fill_request(bench_t *b, uint8_t *buf, uint32_t seq)
{
    memset(buf, 0, DAP_PACKET_SIZE);

    if (b->write_mode) {
        buf[0] = 0x06U;                    /* DAP_TransferBlock */
        buf[1] = 0x00U;                    /* DAP index */
        put_le16(&buf[2], (uint16_t)b->write_words);
        buf[4] = 0x0DU;                    /* AP write, auto-increment DRW */

        for (int i = 0; i < b->write_words; i++) {
            put_le32(&buf[5 + (4 * i)], (seq * 0x01010101U) + (uint32_t)i);
        }
        return;
    }

    buf[0] = BENCH_CMD;
    put_le16(&buf[1], (uint16_t)seq);
    buf[3] = (uint8_t)b->resp_len;
}

static void validate_response(bench_t *b, const uint8_t *resp, int len)
{
    uint16_t seq = (uint16_t)(resp[1] | ((uint16_t)resp[2] << 8));

    if (b->write_mode) {
        if ((len != b->resp_len) || (resp[0] != 0x06U) ||
            (resp[3] != 0x01U) ||
            ((uint16_t)(resp[1] | ((uint16_t)resp[2] << 8)) != (uint16_t)b->write_words)) {
            if (b->errors == 0) {
                fprintf(stderr, "unexpected write response: len %d, cmd 0x%02X, count %u, status 0x%02X\n",
                        len, resp[0], (unsigned)(resp[1] | ((uint16_t)resp[2] << 8)), resp[3]);
            }
            b->errors++;
            b->stop = 1;
            return;
        }

        b->packets++;
        return;
    }

    if (resp[0] != BENCH_CMD) {
        if (b->errors == 0) {
            fprintf(stderr, "unexpected response 0x%02X, firmware misses the 0x83 bench command\n",
                    resp[0]);
        }
        b->errors++;
        b->stop = 1;
        return;
    }

    if (seq != (uint16_t)b->seq_check) {
        if (b->errors == 0) {
            fprintf(stderr, "sequence mismatch: got %u, expected %u\n",
                    seq, (unsigned)(uint16_t)b->seq_check);
        }
        b->errors++;
        b->stop = 1;
        return;
    }

    b->seq_check++;
    b->packets++;
}

static int submit_out(bench_t *b, int idx);

static int submit_in(bench_t *b, int idx);

static void LIBUSB_CALL bench_out_cb(struct libusb_transfer *xfer)
{
    bench_t *b = (bench_t *)xfer->user_data;
    int idx = (int)((xfer->buffer - b->out_buf) / DAP_PACKET_SIZE);

    b->out_inflight--;
    b->out_done++;

    if ((xfer->status != LIBUSB_TRANSFER_COMPLETED) || (xfer->actual_length != DAP_PACKET_SIZE)) {
        if (!b->draining) {
            if (b->errors == 0) {
                fprintf(stderr, "OUT transfer: %s, %u of %d bytes\n",
                        xfer_status_name(xfer->status), xfer->actual_length, DAP_PACKET_SIZE);
            }
            b->errors++;
            b->stop = 1;
        }
        return;
    }

    b->out_bytes += (uint64_t)xfer->actual_length;

    if (b->refill && !b->stop) {
        submit_out(b, idx);
    }
}

static void LIBUSB_CALL bench_in_cb(struct libusb_transfer *xfer)
{
    bench_t *b = (bench_t *)xfer->user_data;
    int idx = (int)((xfer->buffer - b->in_buf) / b->resp_len);

    b->in_inflight--;
    b->in_done++;

    if ((xfer->status != LIBUSB_TRANSFER_COMPLETED) || (xfer->actual_length != b->resp_len)) {
        if (!b->draining) {
            if (b->errors == 0) {
                fprintf(stderr, "IN transfer: %s, %u of %d bytes\n",
                        xfer_status_name(xfer->status), xfer->actual_length, b->resp_len);
                if ((xfer->actual_length > 0) && (xfer->buffer[0] != BENCH_CMD)) {
                    fprintf(stderr, "  response byte 0x%02X, firmware misses the 0x83 bench command\n",
                            xfer->buffer[0]);
                }
            }
            b->errors++;
            b->stop = 1;
        }
        return;
    }

    b->in_bytes += (uint64_t)xfer->actual_length;
    validate_response(b, xfer->buffer, (int)xfer->actual_length);

    if (b->refill && !b->stop) {
        submit_in(b, idx);
    }
}

static int submit_out(bench_t *b, int idx)
{
    uint8_t *buf = b->out_buf + (size_t)idx * DAP_PACKET_SIZE;

    fill_request(b, buf, b->seq_submit++);
    libusb_fill_bulk_transfer(b->out_xfer[idx], b->dev, b->ep_out, buf, DAP_PACKET_SIZE,
                              bench_out_cb, b, TRANSFER_TIMEOUT_MS);

    if (libusb_submit_transfer(b->out_xfer[idx]) != LIBUSB_SUCCESS) {
        b->errors++;
        b->stop = 1;
        return -1;
    }

    b->out_inflight++;
    return 0;
}

static int submit_in(bench_t *b, int idx)
{
    uint8_t *buf = b->in_buf + (size_t)idx * (size_t)b->resp_len;

    libusb_fill_bulk_transfer(b->in_xfer[idx], b->dev, b->ep_in, buf, b->resp_len,
                              bench_in_cb, b, TRANSFER_TIMEOUT_MS);

    if (libusb_submit_transfer(b->in_xfer[idx]) != LIBUSB_SUCCESS) {
        b->errors++;
        b->stop = 1;
        return -1;
    }

    b->in_inflight++;
    return 0;
}

static void pump_events(int timeout_ms)
{
    struct timeval tv;

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    libusb_handle_events_timeout_completed(NULL, &tv, NULL);
}

static int finished(const bench_t *b, uint64_t end_us)
{
    if (b->stop) {
        return 1;
    }
    if ((b->limit != 0) && (b->packets >= b->limit)) {
        return 1;
    }
    return now_us() >= end_us;
}

/* Responses left in the device ring by a previous run would shift the sequence
 * numbers of this run, so drain them before the measurement starts. Each read
 * asks for a full DAP packet: a short response still completes it, and a full
 * one does not leave bytes behind. */
static void flush_input(bench_t *b)
{
    uint8_t buf[DAP_PACKET_SIZE];
    int transferred;
    int quiet = 0;
    int reads = 0;

    while ((quiet < 2) && (reads < 256)) {
        int rc = libusb_bulk_transfer(b->dev, b->ep_in, buf, DAP_PACKET_SIZE, &transferred, 10);

        if (rc == LIBUSB_SUCCESS) {
            quiet = 0;
            reads++;
        } else {
            quiet++;
        }
    }
}

/* One blocking DAP round trip, used only for the SWD setup sequence. */
static int dap_sync_xfer(bench_t *b, const uint8_t *req, int req_len, uint8_t *resp, int *resp_len)
{
    uint8_t out[DAP_PACKET_SIZE];
    int transferred = 0;
    int rc;

    memset(out, 0, sizeof(out));
    memcpy(out, req, (size_t)req_len);

    rc = libusb_bulk_transfer(b->dev, b->ep_out, out, DAP_PACKET_SIZE, &transferred,
                              TRANSFER_TIMEOUT_MS);
    if ((rc != LIBUSB_SUCCESS) || (transferred != DAP_PACKET_SIZE)) {
        return -1;
    }

    rc = libusb_bulk_transfer(b->dev, b->ep_in, resp, DAP_PACKET_SIZE, &transferred,
                              TRANSFER_TIMEOUT_MS);
    if (rc != LIBUSB_SUCCESS) {
        return -1;
    }

    *resp_len = transferred;
    return 0;
}

/* One DAP_Transfer word access; request carries the APnDP, direction and A[3:2]. */
static int dap_transfer_one(bench_t *b, uint8_t request, uint32_t value, int is_read,
                            uint32_t *read_value)
{
    uint8_t req[8];
    uint8_t resp[DAP_PACKET_SIZE];
    int len;

    req[0] = 0x05U;             /* DAP_Transfer */
    req[1] = 0x00U;             /* DAP index */
    req[2] = 0x01U;             /* one transfer */
    req[3] = request;
    if (!is_read) {
        put_le32(&req[4], value);
    }

    if (dap_sync_xfer(b, req, is_read ? 4 : 8, resp, &len) != 0) {
        return -1;
    }
    if ((len < 3) || (resp[0] != 0x05U) || (resp[1] != 0x01U)) {
        fprintf(stderr, "transfer 0x%02X: len %d, %02X %02X %02X %02X\n", request, len,
                resp[0], resp[1], resp[2], resp[3]);
        return -1;
    }
    if ((resp[2] & 0x0FU) != 0x01U) {       /* DAP_TRANSFER_OK */
        fprintf(stderr, "transfer 0x%02X failed, status 0x%02X\n", request, resp[2]);
        return -1;
    }
    if (is_read) {
        if (len < 7) {
            return -1;
        }
        *read_value = resp[3] | ((uint32_t)resp[4] << 8) | ((uint32_t)resp[5] << 16) |
                      ((uint32_t)resp[6] << 24);
    }

    return 0;
}

/* Bring the SWD link up and point the AHB-AP at the destination address. */
static int dap_swd_init(bench_t *b, uint32_t addr)
{
    uint8_t req[64];
    uint8_t resp[DAP_PACKET_SIZE];
    int len;
    int n = 0;
    uint32_t value = 0;

    req[0] = 0x02U;             /* DAP_Connect */
    req[1] = 0x01U;             /* SWD */
    if ((dap_sync_xfer(b, req, 2, resp, &len) != 0) || (len < 2) || (resp[1] != 0x01U)) {
        fprintf(stderr, "DAP_Connect(SWD) failed\n");
        return -1;
    }

    req[0] = 0x13U;             /* DAP_SWD_Configure */
    req[1] = 0x00U;             /* one turnaround clock, no data phase */
    if ((dap_sync_xfer(b, req, 2, resp, &len) != 0) || (len < 2) || (resp[1] != 0x00U)) {
        fprintf(stderr, "DAP_SWD_Configure failed\n");
        return -1;
    }

    req[0] = 0x11U;             /* DAP_SWJ_Clock */
    put_le32(&req[1], b->swclk);
    if ((dap_sync_xfer(b, req, 5, resp, &len) != 0) || (len < 2) || (resp[1] != 0x00U)) {
        fprintf(stderr, "DAP_SWJ_Clock failed\n");
        return -1;
    }

    /* SWJ sequence: line reset, JTAG-to-SWD, line reset, idle */
    req[0] = 0x12U;             /* DAP_SWJ_Sequence */
    for (int i = 0; i < 7; i++) {
        req[2 + n++] = 0xFFU;
    }
    req[2 + n++] = 0x9EU;
    req[2 + n++] = 0xE7U;
    for (int i = 0; i < 7; i++) {
        req[2 + n++] = 0xFFU;
    }
    req[2 + n++] = 0x00U;
    req[1] = (uint8_t)(8 * n);
    if ((dap_sync_xfer(b, req, 2 + n, resp, &len) != 0) || (len < 2) || (resp[1] != 0x00U)) {
        fprintf(stderr, "DAP_SWJ_Sequence failed\n");
        return -1;
    }

    /* DP IDCODE: confirms that the target answers */
    if (dap_transfer_one(b, 0x02U, 0, 1, &value) != 0) {
        fprintf(stderr, "DP IDCODE read failed\n");
        return -1;
    }
    printf("dpidr   : 0x%08X\n", value);

    /* DP SELECT: AP0, register bank 0 */
    if (dap_transfer_one(b, 0x08U, 0x00000000U, 0, NULL) != 0) {
        fprintf(stderr, "DP SELECT write failed\n");
        return -1;
    }

    /* DP CTRL/STAT: request debug and system power */
    if (dap_transfer_one(b, 0x04U, 0x50000000U, 0, NULL) != 0) {
        fprintf(stderr, "DP CTRL/STAT write failed\n");
        return -1;
    }

    for (int i = 0; i < 100; i++) {
        if (dap_transfer_one(b, 0x06U, 0, 1, &value) != 0) {
            fprintf(stderr, "DP CTRL/STAT read failed\n");
            return -1;
        }
        if ((value & 0xA0000000U) == 0xA0000000U) {
            break;
        }
    }
    if ((value & 0xA0000000U) != 0xA0000000U) {
        fprintf(stderr, "debug power-up did not complete, CTRL/STAT 0x%08X\n", value);
        return -1;
    }

    /* AP CSW: 32-bit, single auto-increment */
    if (dap_transfer_one(b, 0x01U, 0x23000012U, 0, NULL) != 0) {
        fprintf(stderr, "AP CSW write failed\n");
        return -1;
    }

    /* AP TAR: first destination address */
    if (dap_transfer_one(b, 0x05U, addr, 0, NULL) != 0) {
        fprintf(stderr, "AP TAR write failed\n");
        return -1;
    }

    return 0;
}

static int run_serial(bench_t *b, uint64_t end_us)
{
    uint8_t *req = b->out_buf;
    uint8_t *resp = b->in_buf;
    int transferred;
    int rc;

    while (!finished(b, end_us)) {
        fill_request(b, req, b->seq_submit);
        rc = libusb_bulk_transfer(b->dev, b->ep_out, req, DAP_PACKET_SIZE, &transferred,
                                  TRANSFER_TIMEOUT_MS);
        if (rc != LIBUSB_SUCCESS) {
            fprintf(stderr, "OUT transfer failed: %s\n", libusb_error_name(rc));
            b->errors++;
            return -1;
        }
        if (transferred != DAP_PACKET_SIZE) {
            fprintf(stderr, "OUT accepted %d of %d bytes\n", transferred, DAP_PACKET_SIZE);
            b->errors++;
            return -1;
        }
        b->seq_submit++;
        b->out_bytes += (uint64_t)transferred;

        rc = libusb_bulk_transfer(b->dev, b->ep_in, resp, b->resp_len, &transferred,
                                  TRANSFER_TIMEOUT_MS);
        if (rc != LIBUSB_SUCCESS) {
            fprintf(stderr, "IN transfer failed: %s\n", libusb_error_name(rc));
            b->errors++;
            return -1;
        }
        if (transferred != b->resp_len) {
            fprintf(stderr, "IN returned %d of %d bytes, firmware misses the 0x83 bench command\n",
                    transferred, b->resp_len);
            if (transferred > 0) {
                fprintf(stderr, "  first byte 0x%02X\n", resp[0]);
            }
            b->errors++;
            return -1;
        }

        b->in_bytes += (uint64_t)transferred;
        validate_response(b, resp, transferred);
    }

    return b->stop ? -1 : 0;
}

static int wait_transfers(bench_t *b, int *done, int target, uint64_t timeout_us)
{
    uint64_t deadline = now_us() + timeout_us;

    while ((*done < target) && !b->stop && (now_us() < deadline)) {
        pump_events(5);
    }

    return (*done == target) ? 0 : -1;
}

static int run_batch(bench_t *b, uint64_t end_us)
{
    while (!finished(b, end_us)) {
        int i;

        b->out_done = 0;
        for (i = 0; (i < b->depth) && !b->stop; i++) {
            if (submit_out(b, i) != 0) {
                break;
            }
        }
        if (wait_transfers(b, &b->out_done, b->depth, 2000000ULL) != 0) {
            break;
        }

        b->in_done = 0;
        for (i = 0; (i < b->depth) && !b->stop; i++) {
            submit_in(b, i);
        }
        if (wait_transfers(b, &b->in_done, b->depth, 2000000ULL) != 0) {
            break;
        }
    }

    return b->stop ? -1 : 0;
}

static void drain(bench_t *b)
{
    uint64_t deadline = now_us() + DRAIN_TIMEOUT_MS * 1000ULL;
    int i;

    b->draining = 1;

    while (((b->out_inflight > 0) || (b->in_inflight > 0)) && (now_us() < deadline)) {
        pump_events(10);
    }

    if ((b->out_inflight > 0) || (b->in_inflight > 0)) {
        for (i = 0; i < b->depth; i++) {
            libusb_cancel_transfer(b->out_xfer[i]);
            libusb_cancel_transfer(b->in_xfer[i]);
        }

        deadline = now_us() + 500000ULL;
        while (((b->out_inflight > 0) || (b->in_inflight > 0)) && (now_us() < deadline)) {
            pump_events(10);
        }
    }

    b->draining = 0;
}

static int run_async(bench_t *b, uint64_t end_us)
{
    int i;

    b->refill = 1;
    for (i = 0; (i < b->depth) && !b->stop; i++) {
        submit_out(b, i);
        submit_in(b, i);
    }

    while (!finished(b, end_us)) {
        pump_events(5);
    }

    b->refill = 0;
    b->stop = 1;
    drain(b);

    return (b->errors != 0) ? -1 : 0;
}

static int find_endpoints(libusb_device_handle *dev, int itf_num, uint8_t *ep_out, uint8_t *ep_in)
{
    struct libusb_config_descriptor *config = NULL;
    int found_out = 0;
    int found_in = 0;
    int rc;

    rc = libusb_get_active_config_descriptor(libusb_get_device(dev), &config);
    if (rc != LIBUSB_SUCCESS) {
        rc = libusb_get_config_descriptor(libusb_get_device(dev), 0, &config);
        if (rc != LIBUSB_SUCCESS) {
            return rc;
        }
    }

    for (unsigned i = 0; (i < config->bNumInterfaces) && !(found_out && found_in); i++) {
        const struct libusb_interface *itf = &config->interface[i];

        for (int a = 0; (a < itf->num_altsetting) && !(found_out && found_in); a++) {
            const struct libusb_interface_descriptor *alt = &itf->altsetting[a];

            if (alt->bInterfaceNumber != (uint8_t)itf_num) {
                continue;
            }

            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];

                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }

                if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
                    *ep_in = ep->bEndpointAddress;
                    found_in = 1;
                } else {
                    *ep_out = ep->bEndpointAddress;
                    found_out = 1;
                }
            }
        }
    }

    libusb_free_config_descriptor(config);

    return (found_in && found_out) ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

static void print_endpoints(const char *prefix, const struct libusb_interface_descriptor *alt)
{
    for (int e = 0; e < alt->bNumEndpoints; e++) {
        const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];

        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) {
            continue;
        }

        printf("    %s ep 0x%02X bulk %s, max packet %u\n", prefix, ep->bEndpointAddress,
               ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) ? "IN" : "OUT",
               ep->wMaxPacketSize);
    }
}

static int list_devices(uint16_t vid, uint16_t pid)
{
    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(NULL, &list);
    int matches = 0;

    if (count < 0) {
        fprintf(stderr, "cannot enumerate USB devices: %s\n", libusb_error_name((int)count));
        return 1;
    }

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS) {
            continue;
        }

        if ((desc.idVendor != vid) || (desc.idProduct != pid)) {
            continue;
        }

        matches++;
        printf("device %04X:%04X at bus %u address %u\n", desc.idVendor, desc.idProduct,
               libusb_get_bus_number(list[i]), libusb_get_device_address(list[i]));

        struct libusb_config_descriptor *config = NULL;

        if (libusb_get_active_config_descriptor(list[i], &config) != LIBUSB_SUCCESS) {
            libusb_get_config_descriptor(list[i], 0, &config);
        }

        if (config != NULL) {
            for (unsigned itf = 0; itf < config->bNumInterfaces; itf++) {
                const struct libusb_interface *intf = &config->interface[itf];
                const struct libusb_interface_descriptor *alt = &intf->altsetting[0];

                printf("  interface %u: class 0x%02X subclass 0x%02X protocol 0x%02X\n",
                       alt->bInterfaceNumber, alt->bInterfaceClass, alt->bInterfaceSubClass,
                       alt->bInterfaceProtocol);
                print_endpoints("endpoint", alt);
            }
            libusb_free_config_descriptor(config);
        }
    }

    libusb_free_device_list(list, 1);

    if (matches == 0) {
        printf("no device %04X:%04X found\n", vid, pid);
        return 1;
    }

    return 0;
}

static libusb_device_handle *bench_open(const options_t *opt, uint8_t *ep_out, uint8_t *ep_in)
{
    libusb_device **list = NULL;
    libusb_device_handle *dev = NULL;
    ssize_t count = libusb_get_device_list(NULL, &list);

    if (count < 0) {
        fprintf(stderr, "cannot enumerate USB devices: %s\n", libusb_error_name((int)count));
        return NULL;
    }

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;

        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS) {
            continue;
        }

        if ((desc.idVendor != opt->vid) || (desc.idProduct != opt->pid)) {
            continue;
        }

        if (libusb_open(list[i], &dev) != LIBUSB_SUCCESS) {
            dev = NULL;
            continue;
        }
        break;
    }

    libusb_free_device_list(list, 1);

    if (dev == NULL) {
        fprintf(stderr, "cannot open device %04X:%04X\n", opt->vid, opt->pid);
        return NULL;
    }

    /* Only the Linux backend supports this; Windows returns NOT_SUPPORTED */
    (void)libusb_set_auto_detach_kernel_driver(dev, 1);

    if (libusb_claim_interface(dev, opt->interface) != LIBUSB_SUCCESS) {
        fprintf(stderr, "cannot claim interface %d, check the WinUSB driver binding\n",
                opt->interface);
        libusb_close(dev);
        return NULL;
    }

    if (find_endpoints(dev, opt->interface, ep_out, ep_in) != 0) {
        fprintf(stderr, "no bulk endpoints on interface %d\n", opt->interface);
        libusb_release_interface(dev, opt->interface);
        libusb_close(dev);
        return NULL;
    }

    return dev;
}

static int bench_alloc(bench_t *b)
{
    b->out_buf = (uint8_t *)calloc((size_t)b->depth * DAP_PACKET_SIZE, 1);
    b->in_buf = (uint8_t *)calloc((size_t)b->depth * (size_t)b->resp_len, 1);
    b->out_xfer = (struct libusb_transfer **)calloc((size_t)b->depth, sizeof(*b->out_xfer));
    b->in_xfer = (struct libusb_transfer **)calloc((size_t)b->depth, sizeof(*b->in_xfer));

    if ((b->out_buf == NULL) || (b->in_buf == NULL) ||
        (b->out_xfer == NULL) || (b->in_xfer == NULL)) {
        return -1;
    }

    for (int i = 0; i < b->depth; i++) {
        b->out_xfer[i] = libusb_alloc_transfer(0);
        b->in_xfer[i] = libusb_alloc_transfer(0);
        if ((b->out_xfer[i] == NULL) || (b->in_xfer[i] == NULL)) {
            return -1;
        }
    }

    return 0;
}

static void bench_free(bench_t *b)
{
    if (b->out_xfer != NULL) {
        for (int i = 0; i < b->depth; i++) {
            libusb_free_transfer(b->out_xfer[i]);
        }
    }
    if (b->in_xfer != NULL) {
        for (int i = 0; i < b->depth; i++) {
            libusb_free_transfer(b->in_xfer[i]);
        }
    }

    free(b->out_buf);
    free(b->in_buf);
    free(b->out_xfer);
    free(b->in_xfer);

    memset(b, 0, sizeof(*b));
}

static void report(const bench_t *b, uint64_t elapsed_us)
{
    double seconds = (double)elapsed_us / 1e6;
    double out_mbs = 0.0;
    double in_mbs = 0.0;

    if (elapsed_us != 0) {
        /* bytes per microsecond equals megabytes per second */
        out_mbs = (double)b->out_bytes / (double)elapsed_us;
        in_mbs = (double)b->in_bytes / (double)elapsed_us;
    }

    printf("\nrequests : %" PRIu64 " (%.0f req/s)\n", b->packets,
           (seconds > 0.0) ? (double)b->packets / seconds : 0.0);
    printf("OUT      : %.2f MB, %.1f KB/s\n", (double)b->out_bytes / 1e6, out_mbs * 1000.0);
    printf("IN       : %.2f MB, %.1f KB/s\n", (double)b->in_bytes / 1e6, in_mbs * 1000.0);
    printf("total    : %.1f KB/s\n", (out_mbs + in_mbs) * 1000.0);
    if (b->write_mode && (elapsed_us != 0)) {
        double payload = (double)b->packets * (double)b->write_words * 4.0;
        printf("payload  : %.2f MB, %.1f KB/s (%d B per request)\n",
               payload / 1e6, payload / (double)elapsed_us * 1000.0, b->write_words * 4);
    }
    printf("errors   : %" PRIu64 "\n", b->errors);
}

static void usage(const char *prog)
{
    printf("usage: %s [options]\n"
           "  -l, --list              list matching devices\n"
           "  -m, --mode <mode>       serial | batch | async | swd (default async)\n"
           "  -d, --depth <n>         transfers kept in flight, batch/async/swd (default %d)\n"
           "  -r, --resp <n>          response bytes per request, %d..%d (default %d)\n"
           "  -t, --time <s>          run time in seconds (default %d)\n"
           "  -n, --packets <n>       stop after n requests (default 0: time only)\n"
           "  -i, --interface <n>     interface number (default 0)\n"
           "      --addr <hex>        swd mode destination address (default %08X)\n"
           "      --words <n>         swd mode words per packet, 1..14 (default %d)\n"
           "      --vid <hex>         vendor id (default %04X)\n"
           "      --pid <hex>         product id (default %04X)\n"
           "  -h, --help              show this text\n",
           prog, DEFAULT_DEPTH, BENCH_MIN_RESP, BENCH_MAX_RESP, DEFAULT_RESP_LEN,
           DEFAULT_SECONDS, DEFAULT_WRITE_ADDR, DEFAULT_WRITE_WORDS, DEFAULT_VID, DEFAULT_PID);
}

static const char *take_arg(int argc, char **argv, int *i, const char *name)
{
    if ((*i + 1) >= argc) {
        fprintf(stderr, "missing value for %s\n", name);
        return NULL;
    }

    (*i)++;
    return argv[*i];
}

static int parse_options(int argc, char **argv, options_t *opt)
{
    memset(opt, 0, sizeof(*opt));
    opt->mode = MODE_ASYNC;
    opt->depth = DEFAULT_DEPTH;
    opt->resp_len = DEFAULT_RESP_LEN;
    opt->seconds = DEFAULT_SECONDS;
    opt->interface = 0;
    opt->vid = DEFAULT_VID;
    opt->pid = DEFAULT_PID;
    opt->write_addr = DEFAULT_WRITE_ADDR;
    opt->swclk = DEFAULT_SWCLK_HZ;
    opt->write_words = DEFAULT_WRITE_WORDS;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value;

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(argv[0]);
            return 1;
        } else if (!strcmp(arg, "-l") || !strcmp(arg, "--list")) {
            opt->list = 1;
        } else if (!strcmp(arg, "-m") || !strcmp(arg, "--mode")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            if (!strcmp(value, "serial")) {
                opt->mode = MODE_SERIAL;
            } else if (!strcmp(value, "batch")) {
                opt->mode = MODE_BATCH;
            } else if (!strcmp(value, "async")) {
                opt->mode = MODE_ASYNC;
            } else if (!strcmp(value, "swd")) {
                opt->mode = MODE_DAP_WRITE;
            } else {
                fprintf(stderr, "unknown mode '%s'\n", value);
                return -1;
            }
        } else if (!strcmp(arg, "-d") || !strcmp(arg, "--depth")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            opt->depth = atoi(value);
        } else if (!strcmp(arg, "-r") || !strcmp(arg, "--resp")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            opt->resp_len = atoi(value);
        } else if (!strcmp(arg, "-t") || !strcmp(arg, "--time")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            opt->seconds = atoi(value);
        } else if (!strcmp(arg, "-n") || !strcmp(arg, "--packets")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            opt->limit = strtoull(value, NULL, 10);
        } else if (!strcmp(arg, "-i") || !strcmp(arg, "--interface")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            opt->interface = atoi(value);
        } else if (!strcmp(arg, "--vid")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            opt->vid = (uint16_t)strtoul(value, NULL, 16);
        } else if (!strcmp(arg, "--pid")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            opt->pid = (uint16_t)strtoul(value, NULL, 16);
        } else if (!strcmp(arg, "--addr")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            opt->write_addr = (uint32_t)strtoul(value, NULL, 16);
        } else if (!strcmp(arg, "--words")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            opt->write_words = atoi(value);
        } else if (!strcmp(arg, "--swclk")) {
            value = take_arg(argc, argv, &i, arg);
            if (value == NULL) {
                return -1;
            }
            opt->swclk = (uint32_t)strtoul(value, NULL, 10);
        } else {
            fprintf(stderr, "unknown option '%s'\n", arg);
            usage(argv[0]);
            return -1;
        }
    }

    if ((opt->depth < 1) || (opt->depth > 64)) {
        fprintf(stderr, "depth must be 1..64\n");
        return -1;
    }
    if ((opt->resp_len < BENCH_MIN_RESP) || (opt->resp_len > BENCH_MAX_RESP)) {
        fprintf(stderr, "response length must be %d..%d\n", BENCH_MIN_RESP, BENCH_MAX_RESP);
        return -1;
    }
    if (opt->mode == MODE_DAP_WRITE) {
        if ((opt->write_words < 1) || (opt->write_words > 14)) {
            fprintf(stderr, "words must be 1..14, 5 + 4*words must fit in %d bytes\n",
                    DAP_PACKET_SIZE);
            return -1;
        }
        opt->resp_len = 4;
    }
    if ((opt->seconds <= 0) && (opt->limit == 0)) {
        fprintf(stderr, "time must be positive\n");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    options_t opt;
    bench_t bench;
    uint64_t start_us;
    uint64_t end_us;
    int rc;

    rc = parse_options(argc, argv, &opt);
    if (rc != 0) {
        return (rc > 0) ? 0 : 1;
    }

    if (libusb_init(NULL) != LIBUSB_SUCCESS) {
        fprintf(stderr, "cannot initialise libusb\n");
        return 1;
    }

    if (opt.list) {
        rc = list_devices(opt.vid, opt.pid);
        libusb_exit(NULL);
        return rc;
    }

    memset(&bench, 0, sizeof(bench));
    bench.mode = opt.mode;
    bench.depth = opt.depth;
    bench.resp_len = opt.resp_len;
    bench.limit = opt.limit;
    bench.write_mode = (opt.mode == MODE_DAP_WRITE) ? 1 : 0;
    bench.write_addr = opt.write_addr;
    bench.swclk = opt.swclk;
    bench.write_words = opt.write_words;

    bench.dev = bench_open(&opt, &bench.ep_out, &bench.ep_in);
    if (bench.dev == NULL) {
        libusb_exit(NULL);
        return 1;
    }

    if (bench_alloc(&bench) != 0) {
        fprintf(stderr, "out of memory\n");
        libusb_release_interface(bench.dev, opt.interface);
        libusb_close(bench.dev);
        libusb_exit(NULL);
        return 1;
    }

    printf("device  : %04X:%04X interface %d, OUT 0x%02X IN 0x%02X\n", opt.vid, opt.pid,
           opt.interface, bench.ep_out, bench.ep_in);
    if (bench.write_mode) {
        printf("mode    : swd-write, depth %d, %d words (%d B) per packet, addr 0x%08X, %u kHz\n",
               bench.depth, bench.write_words, bench.write_words * 4, bench.write_addr,
               bench.swclk / 1000U);
    } else if (bench.mode == MODE_SERIAL) {
        printf("mode    : serial, request %d B, response %d B\n", DAP_PACKET_SIZE, bench.resp_len);
    } else {
        printf("mode    : %s, depth %d, request %d B, response %d B\n", mode_name(bench.mode),
               bench.depth, DAP_PACKET_SIZE, bench.resp_len);
    }

    flush_input(&bench);

    if (bench.write_mode && (dap_swd_init(&bench, bench.write_addr) != 0)) {
        libusb_release_interface(bench.dev, opt.interface);
        libusb_close(bench.dev);
        bench_free(&bench);
        libusb_exit(NULL);
        return 1;
    }

    start_us = now_us();
    end_us = start_us + (uint64_t)opt.seconds * 1000000ULL;

    switch (bench.mode) {
    case MODE_SERIAL:
        rc = run_serial(&bench, end_us);
        break;
    case MODE_BATCH:
        rc = run_batch(&bench, end_us);
        break;
    default:
        rc = run_async(&bench, end_us);
        break;
    }

    report(&bench, now_us() - start_us);

    libusb_release_interface(bench.dev, opt.interface);
    libusb_close(bench.dev);
    bench_free(&bench);
    libusb_exit(NULL);

    return (rc == 0) ? 0 : 1;
}

#if defined(__MINGW32__)
/* The prebuilt libusb archive was linked against the mingw-w64 runtime, which
 * routes vsnprintf() through this helper. MinGW.org lacks it, so provide it. */
int __ms_vsnprintf(char *buffer, size_t count, const char *format, va_list arg)
{
    return vsnprintf(buffer, count, format, arg);
}
#endif
