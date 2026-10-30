# USB Bulk 吞吐测试

不经过 SWD，直接测 RP2350 的 USB 外设、TinyUSB 与 DAP 端点环的搬运能力。固件侧增加一个不解析负载的 vendor 命令，主机侧用一个可配置在途窗口的工具压测。

## 1. 固件命令 0x83

请求固定占用一个 `DAP_PACKET_SIZE`（64 字节）的 DAP 包：

| 偏移 | 字节数 | 内容 |
| --- | --- | --- |
| 0 | 1 | `0x83`（`ID_DAP_Vendor3`） |
| 1 | 2 | 序号，小端 |
| 3 | 1 | 期望的响应长度，4..64，越界按 4 处理 |
| 4 | 60 | 任意数据，固件不读取 |

响应：

| 偏移 | 字节数 | 内容 |
| --- | --- | --- |
| 0 | 1 | `0x83` |
| 1 | 2 | 序号回显 |
| 3 | 1 | `0x00` |
| 4 | n-4 | `0x00` |

命令返回的请求长度为 64、响应长度为请求指定的 n（默认 4），因此上层按"吃满整包"处理；负载不解包、不校验、不拷贝，仅用于占满 USB 通路。

该命令只用于单命令整包发送，不能放进 `ID_DAP_QueueCommands` 批量队列：批量解析按每条命令返回的请求长度前进，本命令固定声明 64 字节，会越过当前包。

## 2. 主机工具

`tools/usb_bulk_bench`，用目录内的 libusb 静态库编译：

```text
make -C tools/usb_bulk_bench
```

参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `-m, --mode` | `async` | `serial` / `batch` / `async` / `swd` |
| `-d, --depth` | 8 | 在途传输数，`batch`/`async`/`swd` 使用 |
| `-r, --resp` | 4 | 每个请求的响应字节数，4..64 |
| `-t, --time` | 5 | 运行时长，秒 |
| `-n, --packets` | 0 | 达到包数后停止，0 表示只按时间 |
| `-i, --interface` | 0 | DAP bulk 接口号 |
| `--addr` | `24000000` | `swd` 模式的写入地址 |
| `--words` | 14 | `swd` 模式每包字数，1..14（56 字节） |
| `--swclk` | 20000000 | `swd` 模式设置的 SWCLK，Hz |
| `--vid` / `--pid` | `2e8a` / `000c` | 设备 ID |
| `-l, --list` | | 列出匹配设备及端点 |

三种纯 USB 模式对应不同的主机提交方式：

| 模式 | 行为 | 对应 |
| --- | --- | --- |
| `serial` | 每包先写后读，只有 1 个请求在途 | OpenOCD 当前行为 |
| `batch` | 一次提交 depth 个 OUT，完成后再提交 depth 个 IN | 删除伪轮询并加大窗口后的 OpenOCD |
| `async` | OUT/IN 各保持 depth 个在途，完成即补 | 异步 bulk 后端 |

`swd` 模式复用 `async` 的提交方式，但把负载换成真实的 `DAP_TransferBlock` 写包。

## 3. 运行

```text
tools\usb_bulk_bench\usb_bulk_bench.exe -l
tools\usb_bulk_bench\usb_bulk_bench.exe -m serial -t 5
tools\usb_bulk_bench\usb_bulk_bench.exe -m batch -d 8 -t 5
tools\usb_bulk_bench\usb_bulk_bench.exe -m async -d 8 -t 10
```

输出 requests、OUT、IN、total 与 errors。每个响应回显请求序号，序号不连续会直接报错，不会静默少算。

## 4. 判读

每个请求在总线上固定是 1 个 64 字节 OUT 加 1 个 n 字节 IN。全速线速 12 Mbit/s，一个整 64 字节事务约占 52 µs，因此“每帧最多 19 个 bulk 事务”只对整包成立；4 字节的短响应只占一小段线速时间，实测可以跑到约 20 事务/ms。

- `serial` 每包一次同步往返，约 5200 请求/秒（351 KB/s），明显低于其它两种模式；
- `batch` 每轮等齐全部 OUT 再等 IN，轮次之间有空隙，约 6800 请求/秒（468 KB/s）；
- `async` 双向各保持多个在途，约 10000 请求/秒（OUT 647 KB/s、合计 687 KB/s）；
- 4 字节响应时每请求约 99 µs，其中 64 字节 OUT 事务本身约占 52 µs，其余是短 IN 事务、包间隔与帧边界；
- 加大响应会摊薄每请求的固定开销：r=64 时请求数降到约 8000，两方向合计升到 1023 KB/s。

`-r` 调整响应字节数，可观察两个方向相互挤占；`-d` 在 `async` 下从 1 加到 16 只改变测量抖动（±4%）范围内的数值，在途窗口不是限制项。

## 5. 交叉验证

`config/DAP_config.h` 中把 `DAP_STATS_ENABLE` 置 1 后，可通过 `0x82` 命令读取端点统计：

```text
cmsis-dap cmd 0x82 0   # 峰值深度
cmsis-dap cmd 0x82 1   # OUT 包数
cmsis-dap cmd 0x82 5   # 命令总耗时
```

深度峰值应随 depth 增大，并在设备被压满后饱和；命令占用率接近 100% 表示设备已被压满。

## 6. 实测参考

Windows 主机、`-t 5`，固件按 `RelWithDebInfo` 构建并把代码与常量复制到 SRAM 执行（见 §8）：

| 模式 | depth | 响应 | 请求/秒 | OUT | IN | 合计 |
| --- | --- | --- | --- | --- | --- | --- |
| serial | 1 | 4 B | 5162 | 330.3 KB/s | 20.6 KB/s | 351.0 KB/s |
| batch | 4 | 4 B | 6693 | 428.3 KB/s | 26.8 KB/s | 455.1 KB/s |
| batch | 8 | 4 B | 6929 | 443.4 KB/s | 27.7 KB/s | 471.1 KB/s |
| async | 1 | 4 B | 10103 | 647.0 KB/s | 40.4 KB/s | 687.4 KB/s |
| async | 2 | 4 B | 10430 | 667.6 KB/s | 41.7 KB/s | 709.3 KB/s |
| async | 4 | 4 B | 10409 | 666.2 KB/s | 41.6 KB/s | 707.8 KB/s |
| async | 8 | 4 B | 9996 | 639.8 KB/s | 40.0 KB/s | 679.7 KB/s |
| async | 16 | 4 B | 10181 | 651.6 KB/s | 40.7 KB/s | 692.3 KB/s |
| async | 8 | 16 B | 10243 | 655.6 KB/s | 163.9 KB/s | 819.5 KB/s |
| async | 8 | 32 B | 9740 | 623.8 KB/s | 311.7 KB/s | 935.5 KB/s |
| async | 8 | 64 B | 7989 | 511.7 KB/s | 511.3 KB/s | 1023.0 KB/s |

- `serial` 的 depth 不参与提交，固定 1 个请求在途，因此是三种模式里最低的。
- depth 1 到 16 的 `async` 请求率在 9996～10430 之间波动，差异在测量抖动（约 ±4%）内，depth 不是限制项。
- 响应改为 64 字节后，两个方向各约 512 KB/s，合计 1023 KB/s，约为按“每帧 19 个整包事务”估算上限的 84%。
- 加大响应时请求率降幅不大（9996→7989），合计带宽却从 687 升到 1023 KB/s，每请求的固定开销被摊薄。
- `batch` 每轮先等全部 OUT 再等全部 IN，轮次之间有空隙，因此低于 `async`；加大窗口不再改善。
- 空命令的请求率与 20 MHz 的 SWD 写入几乎相同（§7），说明设备处理时间已低于 USB 事务预算，瓶颈在总线与主机提交节奏。

## 7. SWD 写 SRAM 实测

`-m swd` 走真实的 SWD 路径：工具先完成 `DAP_Connect`、`DAP_SWD_Configure`、`DAP_SWJ_Clock`、SWJ 切换序列和 AHB-AP 的 CSW/TAR 配置，然后按与 OpenOCD 相同的 `DAP_TransferBlock` 写包压测。

```text
tools\usb_bulk_bench\usb_bulk_bench.exe -m swd -d 4 -t 5 --addr 24000000 --words 14 --swclk 20000000
```

STM32H7、地址 `0x24000000`、SWCLK 20 MHz、每包 14 字（56 字节）：

| 负载 | depth | 请求/秒 | 载荷速率 |
| --- | --- | --- | --- |
| 0x83 空命令 | 1 | 10103 | — |
| 0x83 空命令 | 4 | 10409 | — |
| TransferBlock 写 | 1 | 9866 | 552 KB/s |
| TransferBlock 写 | 2 | 9799 | 549 KB/s |
| TransferBlock 写 | 4 | 9598 | 538 KB/s |
| TransferBlock 写 | 8 | 9638 | 540 KB/s |

- 20 MHz 下写入比空命令只低 2～3%：14 字 × 45 bit ÷ 20 MHz ≈ 31 µs，小于每请求约 100 µs 的 USB 预算，PIO SWD 的时间被 USB 事务完全掩盖。
- SWCLK 降到 1 MHz 后每包约 630 µs，请求率降到 1267（70.9 KB/s），depth 1 与 depth 4 完全相同。
- 未打补丁的 OpenOCD 0.12.0 每提交一个 block 就阻塞等待一次响应（见 openocd_pipeline.md），加载 256 KB 到 SRAM 约 1.3 s（约 200 KB/s），回读校验另需约 1.4 s；差距来自主机侧提交方式，与固件无关。
- DAP 线程绑在核 1、USB 栈在核 0，上表即该配置下的结果。

### depth 的影响

| depth | 空命令请求/秒 | SWD 写入请求/秒 |
| --- | --- | --- |
| 1 | 10103 | 9866 |
| 2 | 10430 | 9799 |
| 4 | 10409 | 9598 |
| 8 | 9996 | 9638 |
| 16 | 10181 | — |

空命令与 SWD 写入的变化都在测量抖动（约 ±4%）内，depth 不再是需要调的参数。窗口只在设备明显慢于总线时才有意义，而那时加深窗口也不会超过设备本身的速度：SWCLK = 1 MHz 时每包约 630 µs，depth 1 与 4 都是 1267 请求/秒。

## 8. 约束

- 请求必须整 64 字节发送，短包不会补齐。
- 响应小于 64 字节时，每个响应在 USB 上是一个短包，主机一次 IN 传输只能取到一个响应，工具按单响应提交。
- `swd` 模式会真实写目标内存，默认地址 `0x24000000`，仅用于基准测试，不要指向有副作用的寄存器。
- 固件按 O2（`RelWithDebInfo`）构建，并用 `PICO_COPY_TO_RAM` 把代码与常量复制到 SRAM 执行，RAM 占用约 198 KB；USB 端点处理与 DAP 命令执行都不再受 XIP 缓存缺失影响。
