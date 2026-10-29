# OpenOCD CMSIS-DAP v2 流水线缺陷记录

本文记录 OpenOCD 的 CMSIS-DAP v2 后端为什么不使用多包在途、由此产生的现象，以及在主机侧的优化方法。固件本身不需要为此改动。

## 1. 现象

同一份固件、同一段加载（113940 字节）：

| 主机 | 耗时 | 速率 |
| --- | --- | --- |
| Windows（原生） | 1192～1194 ms | 95 KB/s |
| Ubuntu 虚拟机（USB 直通） | 1194 ms | 95 KB/s |

固件统计（`cmsis-dap cmd 0x82 <sel>`）：

| 统计项 | 数值 | 说明 |
| --- | --- | --- |
| IN 包数 `sel 2` | 2389 | 平均每包 0.5 ms |
| 命令总耗时 `sel 5` | 512290 µs | 设备占用率 43%，平均每包 214 µs |
| 请求最小间隔 `sel 7` | 215 µs | 与单包处理时间相当 |
| 实际 SWCLK `sel 9` | 4000 kHz | 与请求的适配器速度一致 |
| 环形深度 `sel 0` | 峰值均为 1 | 从未有第二个包排队 |

两个操作系统结果一致、设备只占 43%，说明瓶颈不在主机 USB 栈的差异，而在**主机软件从不把多个请求同时压进在途窗口**。

## 2. 根因：所谓“非阻塞读”实际是阻塞读

OpenOCD 每凑满一个 block（最多 12 个传输）就提交一次，提交前会先做一次“轮询读”来回收旧响应：

```c
/* src/jtag/drivers/cmsis_dap.c, cmsis_dap_swd_queue_cmd() */
if (pending_fifo[pending_fifo_put_idx].transfer_count == pending_queue_len
         || targetsel_cmd) {
    if (pending_fifo_block_count)
        cmsis_dap_swd_read_process(cmsis_dap_handle, 0);   /* 本意：非阻塞轮询 */

    cmsis_dap_swd_write_from_queue(cmsis_dap_handle);

    if (pending_fifo_block_count >= cmsis_dap_handle->packet_count)
        cmsis_dap_swd_read_process(cmsis_dap_handle, USB_TIMEOUT);
}
```

`timeout_ms` 一路传到 libusb：

```c
/* src/jtag/drivers/cmsis_dap_usb_bulk.c, cmsis_dap_usb_read() */
err = libusb_bulk_transfer(dap->bdata->dev_handle, dap->bdata->ep_in,
                           dap->packet_buffer, dap->packet_size,
                           &transferred, timeout_ms);
```

而 libusb 的约定是 **timeout = 0 表示永久等待**（`libusb.h`：*A value of 0 indicates no timeout*），不是“立即返回”。因此这次“轮询”会一直阻塞到上一包的响应到达。

## 3. 时序

设设备处理一包需要 T（实测约 214 µs）：

```text
写 block N ──► 设备处理 ──► 响应 N
   ▲                          │
   └── 读响应 N（阻塞等待）◄───┘
```

- 每个 block 都是“写 → 阻塞读 → 写 → 阻塞读”，**任何时刻只有一个 block 在途**。
- `MAX_PENDING_REQUESTS`（3）和 `DAP_Info` 上报的包数（8）都不会被用满，固件的多槽环形缓冲自然也压不起来。
- 吞吐被固定为 `1 / (设备处理时间 + USB 往返时间)`。

这就是为什么：

- 加深环形缓冲带来的提升来自“端点不再被 NAK”，而不是“多包排队”；
- 提高 SWD 时钟只能压缩 214 µs 中的一部分，整体提升有限；
- Windows 与 Linux 结果几乎相同；
- 请求深度峰值始终是 1。

## 4. 优化方法

### 4.1 删除提交前的伪轮询（最小改动）

在 `src/jtag/drivers/cmsis_dap.c` 的 `cmsis_dap_swd_queue_cmd()` 中删掉提交前的那次读：

```diff
 	if (pending_fifo[pending_fifo_put_idx].transfer_count == pending_queue_len
 			 || targetsel_cmd) {
-		if (pending_fifo_block_count)
-			cmsis_dap_swd_read_process(cmsis_dap_handle, 0);
-
 		/* Not enough room in the queue. Run the queue. */
 		cmsis_dap_swd_write_from_queue(cmsis_dap_handle);
```

删除后，block 会连续提交，直到 `pending_fifo_block_count >= packet_count` 才在写之后阻塞读一次。写入前环形 FIFO 的占用始终小于 `packet_count`，被写入的槽位一定是空闲的，不会覆盖未读响应。

### 4.2 提高在途窗口（可选）

窗口上限由 `packet_count = MIN(MAX_PENDING_REQUESTS, DAP_Info 上报值)` 决定：

```diff
-#define MAX_PENDING_REQUESTS 3
+#define MAX_PENDING_REQUESTS 8
```

固件上报 8，本地环形 16 槽（可缓存 15 包），8 个在途包加 1 个批量终止包也放得下，无需修改固件。

### 4.3 预期效果

| 指标 | 修改前 | 修改后（预期） |
| --- | --- | --- |
| 请求/响应深度峰值 | 1 | 3（只改 4.1）或 8（同时改 4.2） |
| 设备占用率 | 43% | 接近 100% |
| 吞吐 | 95 KB/s | 150～220 KB/s（受主机同步传输开销限制） |
| 最小 OUT 间隔 | 215 µs | 明显小于 215 µs |

设备被压满后，瓶颈回到固件侧，此时再提高 SWCLK 才有意义。

### 4.4 更彻底的做法：异步 USB 后端

4.1 只消除了“每包一次阻塞读”，每次同步 `libusb_bulk_transfer` 的固定开销仍在。要再往上，需要把 bulk 后端改成异步：一次提交多个 IN/OUT URB，用 `libusb_handle_events_timeout()` 回收完成事件。改动量比 4.1 大得多，属于主机侧重构。

## 5. 验证方法

固件侧统计命令（`0x82`）可以直接确认改动是否生效。该功能默认不编译，需先在 `config/DAP_config.h` 中把 `DAP_STATS_ENABLE` 置 1：

```text
cmsis-dap cmd 0x82 4   # 清零
                       # 执行一次加载/扫描
cmsis-dap cmd 0x82 0   # 深度峰值，改后应大于 1
cmsis-dap cmd 0x82 2   # IN 包数
cmsis-dap cmd 0x82 5   # 命令总耗时，改后占用率应接近 100%
cmsis-dap cmd 0x82 7   # 最小请求间隔，改后应明显下降
```

判读：

- 深度峰值仍为 1：改动没有生效，或主机仍在串行等待；
- 设备占用率接近 100%：主机已经把设备压满，下一步优化固件；
- 最小间隔没有下降：主机每次传输的固定开销仍是瓶颈，需要 4.4 的异步后端。

## 6. 固件侧相关配置

| 项 | 取值 | 说明 |
| --- | --- | --- |
| `DAP_PACKET_COUNT` | 8 | 上报给主机的包数，取自 CMSIS-DAP 配置，不做修改 |
| `DAP_BUFFER_COUNT` | 16 | 本地环形槽位，2 × `DAP_PACKET_COUNT`，可缓存 15 包 |
| `DAP_PACKET_SIZE` | 64 | 全速 bulk 的包长上限 |

上报值与本地槽位分开定义，是为了在不动 CMSIS-DAP 配置的前提下留出足够余量，避免主机压满窗口后再加一个批量终止包导致请求环无槽可用。
