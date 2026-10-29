# 端点统计 Vendor 命令测试

`0x82` 命令读取 USB 端点统计（收发包数、环形深度），用于判断吞吐瓶颈在主机流水线还是设备侧。该命令依赖 USB 运行时状态，无法用 cmock 在主机上覆盖，按本文档在实机上验证。

## 1. 测试环境

| 项目 | 要求 |
| --- | --- |
| 硬件 | 目标板，RP2350A，已枚举为 CMSIS-DAP v2 |
| 上位机 | 支持 `cmsis-dap cmd` 的 OpenOCD |
| 固件 | `build/daplink.uf2` |
| 测试负载 | 一段可重复扫描的 SRAM 区域 |

## 2. 命令格式

请求 `[0x82, sel]`，响应固定 5 字节 `[cmd, b0, b1, b2, b3]`，OpenOCD 打印后 4 字节。

| `sel` | 用途 | 打印含义 |
| --- | --- | --- |
| `0` | 环形深度 | `max_req max_resp cur_req cur_resp`，十进制字节 |
| `1` | OUT 包计数 | 小端 32 位 |
| `2` | IN 包计数 | 小端 32 位 |
| `3` | 毫秒计数 | 1 ms tick，小端 32 位 |
| `4` | 清零 | `00 00 00 00` |
| `5` | 命令总耗时 | 微秒，小端 32 位 |
| `6` | 单条命令最长耗时 | 微秒，小端 32 位 |
| `7` | 请求最小间隔 | 微秒，小端 32 位，无数据时为 0 |
| `8` | 请求最大间隔 | 微秒，小端 32 位 |
| `9` | 实际 SWCLK | kHz，小端 32 位 |

计数在每次 USB 配置时清零。

## 3. 基本读取

```powershell
openocd -c "adapter driver cmsis-dap; transport select swd" -c "adapter speed 5000; init" `
        -c "cmsis-dap cmd 0x82 4" `
        -c "cmsis-dap cmd 0x82 0" `
        -c "cmsis-dap cmd 0x82 1" `
        -c "cmsis-dap cmd 0x82 2" `
        -c "cmsis-dap cmd 0x82 3" `
        -c "shutdown"
```

预期：

| 步骤 | 预期打印 |
| --- | --- |
| 清零 | `00 00 00 00` |
| 读深度 | 各值 ≤ 15；当前深度为 0 或 1 |
| 读包数 | 小端计数，随命令条数递增 |
| 读毫秒 | 递增，约等于开机时长 |
| `0x82 5`（非法 selector） | `ff 00 00 00` |

## 4. 扫描期间的统计

```text
cmsis-dap cmd 0x82 4      # 清零
dump_image ...            # 或其它遍历 SRAM 的扫描
cmsis-dap cmd 0x82 0      # 深度峰值
cmsis-dap cmd 0x82 2      # IN 包数
cmsis-dap cmd 0x82 3      # 毫秒计数
cmsis-dap cmd 0x82 5      # 命令总耗时
cmsis-dap cmd 0x82 7      # 请求最小间隔
cmsis-dap cmd 0x82 9      # 实际 SWCLK
```

预期：

| 指标 | 预期 |
| --- | --- |
| `max_resp` | 当前 OpenOCD v2 后端只保持一个包在途，通常为 1；主机真正流水线时会升到 2 以上 |
| `max_req` | 1…3，DAP 线程处理越快越接近 1 |
| `in_packets` | ≈ 扫描秒数 × 2000（约 95 KB/s 时） |
| 反算吞吐 | `in_packets × 48 B ÷ 毫秒差` 与实测扫描速度相差不超过 ±20% |
| `busy_us ÷ in_packets` | 设备处理单条命令的平均时间；1 MHz SWD 约 600 µs，4 MHz 约 210 µs |
| `busy_us ÷ 毫秒差` | 设备占用率；远低于 100% 说明瓶颈在主机或总线 |
| 最小请求间隔 | 主机成串发包时约 60…100 µs；串行等待时约 200 µs 以上 |

- 每个读包最多 12 个 32 位传输，有效载荷 48 字节；`in_packets` 乘以 48 即为实际搬运的数据量。
- 统计命令本身也计入包数，清零后读到的值会多出 1～2 个包。
- OpenOCD v2 后端串行提交的原因与主机侧优化见 `docs/openocd_pipeline.md`。

## 5. 判读

- **`busy_us ÷ 毫秒差` 接近 100%**：设备是瓶颈，继续查 SWD 时钟或 PIO 开销（比较不同 `adapter speed` 下的 `busy_us ÷ in_packets`）。
- **`busy_us ÷ 毫秒差` 很低**：设备在空转，瓶颈在主机或 USB 往返。
- **`max_resp` 长期只有 1 且吞吐偏低**：主机没有把多个请求排进在途窗口，检查主机侧的队列深度。
- **最小请求间隔远大于 100 µs**：主机每次传输都在串行等待，加在途窗口也不会改善。
- **`max_req` 达到 15**：请求环被排满，此时主机侧会出现 NAK，需要减小在途窗口或加快 DAP 线程处理。

## 6. 回归检查

- 统计命令执行前后，SWD 扫描与 Flash 操作正常。
- `cmake --build build` 退出码为 0 且无新增告警。
- 连续多次读取后计数单调递增，清零后归零。
