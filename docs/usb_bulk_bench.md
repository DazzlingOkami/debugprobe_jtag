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

全速 bulk 每个 1 ms 帧最多 19 个 bulk 事务。本测试每个请求是 1 个 64 字节 OUT 加 1 个短 IN，因此：

- 帧调度上限约 9.5 请求/ms，即 OUT 约 600 KB/s、合计约 640 KB/s；
- `serial` 受每包一次往返限制，吞吐由设备处理时间加主机固定开销决定；
- `batch`/`async` 应接近帧上限，此时瓶颈是 USB 总线本身，而不是端点环；
- 若 `async` 明显低于 `batch`，新的瓶颈在主机侧 URB 提交与回收，或驱动层。

`-r` 调整响应字节数：加大响应会占用更多 IN 方向的事务，可观察两个方向相互挤占。

## 5. 交叉验证

`config/DAP_config.h` 中把 `DAP_STATS_ENABLE` 置 1 后，可通过 `0x82` 命令读取端点统计：

```text
cmsis-dap cmd 0x82 0   # 峰值深度
cmsis-dap cmd 0x82 1   # OUT 包数
cmsis-dap cmd 0x82 5   # 命令总耗时
```

深度峰值应随 depth 增大；命令占用率接近 100% 表示设备已被压满。

## 6. 实测参考

Windows 主机、`-t 5` 的典型结果：

| 模式 | depth | 响应 | 请求/秒 | OUT | IN | 合计 |
| --- | --- | --- | --- | --- | --- | --- |
| serial | 1 | 4 B | 4839 | 309.7 KB/s | 19.4 KB/s | 329.1 KB/s |
| batch | 4 | 4 B | 6105 | 390.7 KB/s | 24.4 KB/s | 415.1 KB/s |
| batch | 8 | 4 B | 6171 | 394.9 KB/s | 24.7 KB/s | 419.6 KB/s |
| async | 4 | 4 B | 8541 | 546.7 KB/s | 34.2 KB/s | 580.8 KB/s |
| async | 8 | 4 B | 8659 | 554.2 KB/s | 34.6 KB/s | 588.9 KB/s |
| async | 8 | 64 B | 7754 | 496.7 KB/s | 496.3 KB/s | 992.9 KB/s |

- depth 不是限制：从 4 加到 8，`batch` 与 `async` 都只提升约 1%（415.1→419.6 KB/s、580.8→588.9 KB/s），depth 16 与 depth 8 完全一致。
- 4 字节响应时，每个请求在总线上是 1 个 64 字节 OUT 加 1 个 4 字节 IN，`async` 达到帧调度上限的约 91%。
- 响应改为 64 字节后，两个方向各约 497 KB/s，OUT 与 IN 平分事务，总带宽约 1 MB/s。
- `batch` 每轮先等全部 OUT 再等全部 IN，轮次之间有空隙，因此低于 `async`；加大窗口不再改善。

## 7. SWD 写 SRAM 实测

`-m swd` 走真实的 SWD 路径：工具先完成 `DAP_Connect`、`DAP_SWD_Configure`、`DAP_SWJ_Clock`、SWJ 切换序列和 AHB-AP 的 CSW/TAR 配置，然后按与 OpenOCD 相同的 `DAP_TransferBlock` 写包压测。

```text
tools\usb_bulk_bench\usb_bulk_bench.exe -m swd -d 4 -t 5 --addr 24000000 --words 14 --swclk 20000000
```

STM32H7、地址 `0x24000000`、SWCLK 20 MHz、每包 14 字（56 字节）：

| 负载 | depth | 请求/秒 | 载荷速率 |
| --- | --- | --- | --- |
| 0x83 空命令 | 1 | 9294 | — |
| 0x83 空命令 | 4 | 8296 | — |
| TransferBlock 写 | 1 | 7230 | 405 KB/s |
| TransferBlock 写 | 2 | 6670 | 373 KB/s |
| TransferBlock 写 | 4 | 6635 | 372 KB/s |
| TransferBlock 写 | 8 | 6721 | 376 KB/s |

同一固件用 OpenOCD 0.12.0+dev 写 256 KB 到 SRAM：684 ms，约 383 KB/s，与自建生成器 depth 4 的结果一致。

- 真实 SWD 写比空命令每包多约 30 µs，与 14 字 × 45 bit ÷ 20 MHz ≈ 31 µs 吻合，说明 PIO SWD 接近线速。
- 478 KB/s 的估算以 8541 请求/秒为前提，而该数字来自不执行 SWD 的空命令；写入时这部分 SWD 时间叠加在 USB 包周期上。

### 为什么 depth 1 反而最快

全速 bulk 每 1 ms 帧最多 19 个事务，一个请求固定占 1 个 OUT 加 1 个 IN，上限约 9500 请求/秒。

| depth | 空命令请求/秒 | 事务槽利用率 |
| --- | --- | --- |
| 1 | 9140 | 96% |
| 2 | 8270 | 87% |
| 4 | 8080 | 85% |
| 8 | 8064 | 85% |

depth 1 时主机严格"发一包、收一包"，每个事务槽都用在有效包上。depth ≥2 时多个 IN 事务同时挂在端点，而设备一次只有一个响应可发，先到的 IN 被 NAK，事务槽被浪费；窗口越深，这种空转越多，效率稳定在 85%。

窗口只在设备明显慢于总线时才有意义。SWCLK 降到 1 MHz 后每包 800 µs，depth 1 与 depth 4 都是约 1240 请求/秒，没有区别。

DAP 线程与 USB 分核运行后：20 MHz 写入 depth 1 为 431.9 KB/s，depth 4 为 381.2 KB/s，结论不变。

## 8. 约束

- 请求必须整 64 字节发送，短包不会补齐。
- 响应小于 64 字节时，每个响应在 USB 上是一个短包，主机一次 IN 传输只能取到一个响应，工具按单响应提交。
- `swd` 模式会真实写目标内存，默认地址 `0x24000000`，仅用于基准测试，不要指向有副作用的寄存器。
- 固件按 O2（`RelWithDebInfo`）构建。
