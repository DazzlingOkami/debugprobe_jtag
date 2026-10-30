# CMSIS-DAP 调试器设计文档

## 1. 概述

本项目是一个基于 RP2350A 的 CMSIS-DAP 兼容调试器固件。调试器通过 USB 与上位机（Keil MDK、OpenOCD、pyOCD 等）通信，通过 PIO 产生的 SWD/JTAG 时序访问目标芯片的调试端口。除调试通道外，固件还提供一路 USB-CDC 转 UART 的串口桥，以及一条 IIC 总线用于配置接口缓冲芯片的参考电压。

固件基于 Pico SDK 2.x、FreeRTOS-Kernel（SMP）、TinyUSB，并复用 ARM 官方 CMSIS-DAP 参考实现（`lib/CMSIS-DAP`）的命令解析层。

### 1.1 主要特性

| 项目 | 实现情况 |
| --- | --- |
| SWD 调试通道 | 已实现（PIO 时序） |
| JTAG 调试通道 | 已实现（PIO 时序，与 SWD 共用状态机） |
| CMSIS-DAP v2（WinUSB Bulk） | 已实现 |
| CMSIS-DAP v1（HID） | 描述符保留，未启用 |
| USB-CDC 转 UART 桥 | 已实现（DMA + 环形缓冲） |
| SWO 跟踪 | 未启用 |
| 接口缓冲参考电压（MCP4716A0） | 已实现（IIC，Vendor 命令配置并写入 EEPROM，上电自动恢复） |
| 状态指示灯 | 已实现（连接灯 GPIO21、运行灯 GPIO22） |
| 看门狗与双核心跳 | 已实现 |

## 2. 硬件平台

### 2.1 主控与存储

| 项目 | 取值 | 来源 |
| --- | --- | --- |
| 芯片 | RP2350A（`PICO_RP2350A = 1`） | `config/dap_board.h` |
| 平台 | `rp2350-arm-s`（Cortex-M33，安全态） | `CMakeLists.txt` |
| 系统时钟 | 150 MHz（SDK 默认 `SYS_CLK_HZ`，与 `CPU_CLOCK` 一致） | `CPU_CLOCK` |
| 内核数 | 2（FreeRTOS SMP） | `config/FreeRTOSConfig.h` |
| Flash | 2 MB | `PICO_FLASH_SIZE_BYTES` |
| 板级定义 | `dap_board`（`PICO_BOARD_HEADER_DIRS = config/`） | `CMakeLists.txt` |

### 2.2 引脚分配

引脚集中在 `config/dap_board.h` 定义。调试接口的物理引脚（TDI/TDO/TCK/DIO 等）由 `probe_oen.pio` 以侧集（sideset）与并行输出方式统一映射到 PIO0。

| 功能 | 宏 | 引脚 | 说明 |
| --- | --- | --- | --- |
| JTAG TDI | `PROBE_JTAG_TDI_PIN` | 0 | PIO 输出；SWD 模式下归 CDC 桥 UART0 TX，JTAG 模式下归 PIO |
| JTAG TDO | `PROBE_JTAG_TDO_PIN` | 2 | PIO 输入，旁路输入同步器 |
| SWD/JTAG 时钟 | `PROBE_SWD_CLK_PIN` | 3 | PIO 侧集输出，CLK |
| SWD 输出使能 | `PROBE_SWD_DOE_PIN` | 4 | PIO 侧集输出，高 = 缓冲器 A 驱动，低 = 高阻 |
| SWD 数据输入 | `PROBE_SWD_DI_PIN` | 5 | PIO 输入，旁路输入同步器 |
| SWD 数据输出 | `PROBE_SWD_DO_PIN` | 6 | PIO 输出；JTAG 模式下作 TMS（set 引脚组） |
| 目标复位 | `PROBE_RESET_PIN` | 7 | 开漏模拟，方向切换 |
| JTAG TRST | `PROBE_JTAG_TRST_PIN` | 9 | 推挽输出，`PIN_nTRST_OUT()` 驱动 |
| IIC SDA | `IIC_SDA_PIN` | 10 | I2C1，400 kHz |
| IIC SCL | `IIC_SCL_PIN` | 11 | I2C1，400 kHz |
| CDC 桥 UART TX（SWD） | `PROBE_UART_TX_SWD_PIN` | 0 | UART0，仅 SWD 模式使用 |
| CDC 桥 UART TX（JTAG） | `PROBE_UART_TX_JTAG_PIN` | 12 | UART0，仅 JTAG 模式使用 |
| UART TX 缓冲器方向 | `PROBE_UART_TX_OE_PIN` | 8 | 高 = GPIO12 缓冲器输出，低 = 输入 |
| 调试串口 TX | `PICO_DEFAULT_UART_TX_PIN` | 20 | UART1 日志输出 |
| 调试串口 RX | `PICO_DEFAULT_UART_RX_PIN` | 25 | 未使用 |
| CDC 桥 UART RX | `PROBE_UART_RX` | 1 | UART0 |
| 连接指示灯 | `PROBE_DAP_CONNECTED_LED` | 21 | 低电平点亮 |
| 运行指示灯 | `PROBE_DAP_RUNNING_LED` | 22 | 低电平点亮 |

> `PICO_PIO_USE_GPIO_BASE 0`：允许 PIO 直接访问 GPIO32–47 区间。

## 3. 系统架构

### 3.1 分层结构

```
┌──────────────────────────────────────────────────────────┐
│ app/main.c          启动流程、看门狗任务                  │
├──────────────────────────────────────────────────────────┤
│ module/usb          TinyUSB 设备栈接入、描述符、回调      │
│ module/dap          ① Bulk 端点收发与 DAP 线程            │
│                     ② SWD/JTAG 的 PIO 适配               │
├──────────────────────────────────────────────────────────┤
│ lib/CMSIS-DAP       DAP_ExecuteCommand 命令解析与 SWD/JTAG│
│                     状态机（上游参考实现，不修改）        │
├──────────────────────────────────────────────────────────┤
│ driver/swd          PIO 位流引擎（probe.c + probe_oen.pio）│
│ driver/iic_shard    I2C1 总线互斥访问                     │
│ driver/uart         CDC↔UART 桥（DMA + 环形缓冲）         │
├──────────────────────────────────────────────────────────┤
│ config              板级定义、CMSIS-DAP 适配、内核与 USB 配置│
│ bsp                 Pico SDK 引入                         │
└──────────────────────────────────────────────────────────┘
```

### 3.2 目录职责

| 路径 | 职责 |
| --- | --- |
| `app/` | 程序入口、启动任务、看门狗喂狗任务 |
| `bsp/` | `pico_sdk_import.cmake`，Pico SDK 引入 |
| `config/` | `dap_board.h` 板级宏、`DAP_config.h` CMSIS-DAP 适配层、`FreeRTOSConfig.h`、`tusb_config.h` |
| `driver/` | 与芯片外设直接交互的底层驱动：`swd`（PIO 位流）、`iic_shard`（IIC1 总线互斥）、`dac_mcp4716`（参考电压 DAC）、`uart`（CDC 桥）。`CMakeLists.txt` 用 `file(GLOB driver/*/*.c)` 收集 |
| `module/dap/` | DAP 端点处理与 PIO 适配：`tusb_edpt_handler.c`、`sw_dp_pio.c`、`jtag_dp_pio.c`、`dap_vendor.c`、`dap_stats.h`、`dap_pio.h` |
| `lib/` | 第三方与上游代码：CMSIS-DAP Firmware、CMSIS_6、FreeRTOS-Kernel |
| `module/` | 与硬件弱相关的功能模块：USB、DAP 端点、UART 桥、日志、工具 |
| `tools/picotool` | Windows 下预置的 picotool 安装，避免联网下载 |

### 3.3 构建系统

- CMake ≥ 3.22，交叉编译器 `arm-none-eabi-gcc`，C99。
- `Preload.cmake` 强制使用 `Unix Makefiles` 生成器。
- 构建目录固定为仓库根目录下的 `build/`。
- 必须设置 `PICO_SDK_PATH` 指向已检出的 SDK；**不要**启用 `PICO_SDK_FETCH_FROM_GIT`。
- Windows 下 `CMakeLists.txt` 将 `picotool_DIR` 指向仓库内 `tools/picotool`。
- 构建依赖 `git`：配置阶段把 `patches/dap_swj_clock_hz.patch` 应用到 `build/generated/` 的 `DAP.c` 副本，上游子模块不被修改；补丁或上游文件变化时自动重新配置。
- 在 `driver/` 下新增目录或 `.c` 文件后必须重新执行 `cmake -S . -B build`，`GLOB` 没有 `CONFIGURE_DEPENDS`。
- 产物：`build/daplink.elf/.bin/.hex/.uf2`。

## 4. 启动流程与任务模型

### 4.1 启动顺序

`app/main.c` 的 `main()` 中，在调度器启动前完成：

1. `bi_decl_config()`：把引脚与接口信息写入二进制信息（`picotool info` 可读）。
2. `setvbuf(stdout, NULL, 0, _IONBF)`：关闭 stdout 缓冲。
3. `board_init()`、`usb_serial_init()`：板级初始化与从 Flash 唯一 ID 生成 USB 序列号。
4. `cdc_uart_init()`：初始化 UART0（TX 默认在 GPIO0）、TX 缓冲器方向脚、环形缓冲、DMA 通道与 DMA_IRQ_0。
5. `tusb_init()`：初始化 TinyUSB 设备栈。
6. `stdio_uart_init()`：日志串口（UART1 / GPIO20）。
7. `DAP_Setup()`：复位 CMSIS-DAP 内部状态（`DAP_Data`），并把连接灯/运行灯配成输出、初始为熄灭。
8. 创建 `start` 任务后调用 `vTaskStartScheduler()`。

调试口引脚不在启动时初始化：`probe_init()` 由 `PORT_SWD_SETUP()` / `PORT_JTAG_SETUP()` 在上位机连接时按需调用（见 `config/DAP_config.h`）。

`start_task` 依次创建看门狗任务、调用 `usb_init()` 创建 USB 线程与 DAP 线程、创建 CDC 线程、初始化 IIC 总线并读取 MCP4716A0 的当前状态（不写寄存器），随后自删除。DAC 读取失败只打印日志，不阻断启动；参考电压由 DAP Vendor 命令配置并写入 EEPROM。

### 4.2 任务列表

| 任务 | 优先级 | 栈（StackType_t） | 创建位置 | 亲和性 |
| --- | --- | --- | --- | --- |
| `start` | 4 | 1024 | `main()` | 未绑定 |
| `WDOG` | 1 | 512 | `start_task()` | 未绑定（轮流遍历各核喂狗） |
| `USB` | 2 | `configMINIMAL_STACK_SIZE`(256) | `usb_init()` | 核 0 |
| `UART` | 3 | 256 | `cdc_thread_create()` | 核 0 |
| `DAP` | 3 | 256 | `usb_init()` | 核 1 |
| IDLE / Tmr Svc | 0 / 31 | — | 内核 | — |

`configUSE_TIME_SLICING = 1`，同优先级任务时间片轮转。堆使用 `heap_4`，`configTOTAL_HEAP_SIZE = 100 KB`。
`configCHECK_FOR_STACK_OVERFLOW = 2`，溢出与内存分配失败都会 `panic()`。

### 4.3 看门狗

`watchdog_task` 在 `start` 任务中创建：

- 若本次复位由看门狗触发，则打印日志并进入 USB Bootloader（`reset_usb_boot(0, 0)`），便于固件失效后重新烧录。
- 使能 1 s 超时看门狗；SMP 配置下轮流把自身亲和性绑定到各核并调用 `watchdog_update()`，每核间隔 600 ms 喂一次，保证两个核都被覆盖。
- DAP 线程绑定在核 1 且优先级高于 `WDOG`，请求密集时会把核 1 占满，`WDOG` 无法在该核运行。因此 DAP 线程在每条命令处理前检查时间戳，每 500 ms 调用一次 `watchdog_update()`；`xTaskNotifyWait()` 的等待点会让出 CPU，`WDOG` 仍可在核 1 运行。

## 5. USB 传输层

### 5.1 描述符与接口布局

配置描述符由 `module/usb/usb_descriptors.c` 定义，共 3 个接口：

| 接口号 | 类型 | 端点 | 用途 |
| --- | --- | --- | --- |
| 0 | Vendor Specific（Bulk） | OUT `0x04` / IN `0x85`，64 字节 | CMSIS-DAP v2 |
| 1 | CDC 通信接口 | 通知 `0x81` | CDC 串口 |
| 2 | CDC 数据接口 | OUT `0x02` / IN `0x83`，64 字节 | CDC 串口 |

设备描述符：`idVendor = 0x2E8A`、`idProduct = 0x000c`、`bcdUSB = 0x0210`。
接口 0 描述符下标 5（"CMSIS-DAP v2 Interface"），厂商字符串为 "Cassini"，产品字符串取 `PROBE_PRODUCT_STRING`（`CMSIS_DAP`），序列号由 Flash 唯一 ID 生成。

为让 Windows 免驱识别 Bulk 接口，固件通过 BOS 描述符提供 **MS OS 2.0** 描述符，为接口 0 写入 `DeviceInterfaceGUIDs = {CDB3B5AD-293B-4663-AA36-1AAE46463776}`（WinUSB），并为 CDC 接口 1/2 写入 `DeviceDesc = "CDC-UART"` 与 `FriendlyName = "CDC-UART"`。

`config/tusb_config.h` 中 HID 与 Vendor 类均编入固件，但配置描述符按 `PROBE_DEBUG_PROTOCOL` 只实例化其中一个。当前 `PROBE_DEBUG_PROTOCOL = PROTO_DAP_V2`，即走 Bulk 通道；切换到 `PROTO_DAP_V1` 则走 HID 中断端点。

### 5.2 DAP 端点处理

`module/dap/tusb_edpt_handler.c` 以自定义 TinyUSB 类驱动（`usbd_app_driver_get_cb()`）的形式接入，`dap_edpt_open()` 校验接口的 Class/SubClass/Protocol 全为 0 后打开两个 Bulk 端点。

双向各维护一个环形缓冲区 `buffer_t`。本地槽位数是 `DAP_BUFFER_COUNT`（= 2 × `DAP_PACKET_COUNT`，即 16），上报给主机的仍是 `DAP_PACKET_COUNT`（8）；这样主机把 8 个包全部排满、再加一个批量终止包也放得下，端点不需要 NAK。

```c
#define DAP_BUFFER_COUNT (DAP_PACKET_COUNT * 2U)

typedef struct {
    uint8_t data[DAP_BUFFER_COUNT][DAP_PACKET_SIZE];
    uint16_t data_len[DAP_BUFFER_COUNT];
    volatile uint32_t wptr;
    volatile uint32_t rptr;
    volatile bool wasFull;
} buffer_t;
```

`data_len` 记录每个响应槽的实际长度，IN 回调按槽重新投递，因此多个响应可以同时在途。

数据流与流控要点：

- **OUT 方向**：`dap_edpt_xfer_cb` 在每次搬运完成后，若缓冲未满就立即补发 `usbd_edpt_xfer()`；若已满则只置 `wasFull`，由 DAP 线程消费后补发。
- **IN 方向**：DAP 线程只投递队首响应，后续响应由 IN 回调在每次完成后链式补发（`rptr != wptr` 表示仍有待发响应），从而支持多包连续发送。
- **流控**：DAP 线程在写响应槽前等待 `buffer_full()` 解除，不会覆盖主机尚未读取的响应。
- 两侧回调通过 `xTaskNotify()` 唤醒阻塞在 `xTaskNotifyWait()` 的 DAP 线程，DAP 线程不再自挂起。
- 环形缓冲索引的读写和端点投递由互斥量 `edpt_mutex` 保护，适配双核 SMP。
- 请求在其槽内直接处理，响应直接写入响应槽，省去中间拷贝。
- 端点处理同时统计收发包数和环形深度峰值，通过 Vendor 命令 `0x82` 读取（见 §6.7）。
- 主机能否用满这些槽位取决于上位机自身的排队策略，OpenOCD v2 后端的问题与优化见 `docs/openocd_pipeline.md`。

### 5.3 原子命令（QueueCommands）

DAP 线程识别 `ID_DAP_QueueCommands` 开头的报文，将其就地改写为 `ID_DAP_ExecuteCommands` 并向后扫描，直到遇到非 Queue 命令才整体交给 `DAP_ExecuteCommand()` 处理。这实现了 CMSIS-DAP 的原子命令序列语义；若后续报文尚未到达，则在 `xTaskNotifyWait()` 中等待 USB 回调唤醒。

### 5.4 USB 挂载/卸载

DAP 线程在 `usb_init()` 中与 USB 线程一起创建，此后一直运行；`tud_mount_cb()`、`tud_unmount_cb()`、`tud_suspend_cb()` 与 `tud_resume_cb()` 只打印日志，不创建、挂起或删除该线程。重新枚举时由 `dap_edpt_open()` 复位收发环形缓冲的索引，DAP 线程继续使用同一份上下文。

`usb_thread` 调用 `tud_task()` 处理事件；未连接或已挂起时按 1 ms 延时，其余情况用 `xTaskNotifyWait()` 休眠。TinyUSB 在事件入队时调用 `tud_event_hook_cb()`（ISR 与任务上下文分别走 `xTaskNotifyFromISR()` 和 `xTaskNotify()`），USB 线程被立即唤醒，端点回调不会再被 1 ms 的 tick 延迟。

## 6. DAP 协议层

### 6.1 上游代码复用方式

`lib/CMSIS-DAP` 为 ARM 官方参考实现，本项目不修改该子模块：`DAP.c` 的本地适配在构建目录生成副本后再编译（见 §6.3），`SWO.c` 按原样编译。
上游的 `SW_DP.c`、`JTAG_DP.c`、`DAP_vendor.c` 不参与编译，改由 `module/dap/` 提供同名实现：

- `SW_DP.c` → `sw_dp_pio.c`：提供 `SWJ_Sequence()`、`SWD_Sequence()`、`SWD_Transfer()`，把逐位 GPIO 翻转替换为 PIO 命令投递。
- `JTAG_DP.c` → `jtag_dp_pio.c`：提供 `JTAG_Sequence()`、`JTAG_IR()`、`JTAG_ReadIDCode()`、`JTAG_WriteAbort()`、`JTAG_Transfer()`，同样是 PIO 命令投递。
- `DAP_vendor.c` → `dap_vendor.c`：提供 `DAP_ProcessVendorCommand()` 与参考电压命令，见 §6.6。

对目标端的全部硬件访问最终收敛到 `config/DAP_config.h` 中的内联函数。其中真正有效的是：

- `PORT_SWD_SETUP()` → `probe_init()` + `probe_enter_swd()`
- `PORT_JTAG_SETUP()` → `probe_init()` + `probe_enter_jtag()`
- `PORT_OFF()` → `probe_deinit()`（空实现，见 §10.2）
- `PIN_SWDIO_OUT_ENABLE()` / `PIN_SWDIO_OUT_DISABLE()` → `probe_write_mode()` / `probe_read_mode()`（空实现：读写方向由 PIO 命令的 side-set/DOE 按拍控制）
- `PIN_nRESET_IN()` / `PIN_nRESET_OUT()` → `probe_reset_level()` / `probe_assert_reset()`
- `PIN_nTRST_OUT()` → 直接驱动 `PROBE_JTAG_TRST_PIN`
- 各 `*_IN()` → `gpio_get()`，供 `DAP_SWJ_Pins` 回读与 wait
- `TIMESTAMP_GET()` → `time_us_32()`

`PIN_SWCLK_TCK_SET/CLR`、`PIN_SWDIO_TMS_SET/CLR`、`PIN_SWDIO_OUT/IN`、`PIN_TDI_OUT` 仍为空实现——SWD/JTAG 时序完全由 PIO 承担，这些宏不再参与实际路径。

### 6.2 关键配置项

| 宏 | 取值 | 影响 |
| --- | --- | --- |
| `DAP_SWD` | 1 | 支持 SWD 模式 |
| `DAP_JTAG` | 1 | 支持 JTAG 模式 |
| `DAP_JTAG_DEV_CNT` | 8 | `DAP_Data.jtag_dev` 中 IR 长度表容量 |
| `DAP_DEFAULT_PORT` | 1 | 默认 SWD |
| `DAP_DEFAULT_SWJ_CLOCK` | 1 MHz | 默认调试时钟 |
| `CPU_CLOCK` | 150 MHz | 与 `clk_sys` 一致，供 `Delayms()` / `DAP_Delay()` 换算忙等次数 |
| `DAP_PACKET_SIZE` / `COUNT` | 64 / 8 | 单包 64 字节，上报 8 个包；本地环形 16 槽（`DAP_BUFFER_COUNT`） |
| `SWO_UART` / `SWO_MANCHESTER` | 0 / 0 | SWO 未启用 |
| `DAP_UART` / `DAP_UART_USB_COM_PORT` | 0 / 0 | DAP 内建 UART 未启用（串口走独立 CDC 接口） |
| `TIMESTAMP_CLOCK` | 1 MHz | 基于 `time_us_32()` |

### 6.3 时钟换算链路

`DAP_SWJ_Clock` 携带 1 Hz 分辨率的请求值。`lib/CMSIS-DAP` 的上游 `Set_Clock_Delay()` 会把它量化为整数 `clock_delay`，因此构建时用 `patches/dap_swj_clock_hz.patch` 在 `build/generated/` 生成一份 `DAP.c` 副本，把这个函数替换为：

```c
static void Set_Clock_Delay(uint32_t clock) {
  probe_set_swclk_freq_hz(clock);
}
```

同一补丁只做这一处替换，`DAP_Info` 上报的 `DAP_PACKET_COUNT` 保持 CMSIS-DAP 配置值不变；本地的多槽缓冲通过 `DAP_BUFFER_COUNT` 单独定义（见 §5.2）。

原始 `lib/CMSIS-DAP` 子模块不做修改，也不参与编译。补丁由 `lib/CMakeLists.txt` 在配置阶段应用，补丁或上游文件变化时自动重新生成。

`probe_set_swclk_freq_hz()` 计算 PIO 分频值：`divider = clk_sys / swclk / 4`（PIO 程序中 SWCLK 周期为 4 个 SM 执行周期），以 1/256 为单位向上取整，保证实际频率不大于请求值。输入频率钳在 `PROBE_SWCLK_MIN_KHZ`（10 kHz）以上，分频系数钳在 `[1, 65536]`：16:8 分频器的整数部分为 0 表示 65536。写入后按分频值反算真实频率存入 `swclk_actual_freq`，供 `probe_get_swclk_freq()` 读取；分频值同时决定握手超时上限（见 §7.2）。

分频寄存器在 `DAP_SWJ_Clock` 处理时立即更新，SWD/JTAG 操作入口不再检查 `clock_delay`。`probe_init()` 初始化状态机后会重新下发最近一次请求的频率，因此连接前设置的时钟不会丢失。未下发时钟时的默认值由 `probe.h` 的 `PROBE_SWCLK_DEFAULT_HZ` 提供，`DAP_DEFAULT_SWJ_CLOCK` 引用该宏，probe 层不反向依赖 DAP 配置。

16:8 分频器在 150 MHz `clk_sys` 下的相对分辨率为 `1 / (256 × divider)`：25 MHz 附近约 0.26%，37.5 MHz 处约 0.39%，1 MHz 处约 0.01%。实测方法见 `docs/test_swj_clock.md`。

### 6.4 `DAP_Info` 字符串

四个字符串回调在 `config/DAP_config.h` 中实现，取值来自 `config/dap_board.h` 的板级宏：

| 信息项 | 返回值 | 来源 |
| --- | --- | --- |
| Vendor | `Cassini` | `PROBE_VENDOR_STRING` |
| Product | `CMSIS_DAP` | `PROBE_PRODUCT_STRING` |
| Serial Number | Flash 唯一 ID 的 16 位十六进制串 | `usb_serial`，与 USB 描述符同源 |
| Product Firmware Version | `2.10` | `PROBE_FIRMWARE_VERSION`，对应设备描述符 `bcdDevice = 0x0210` |

返回值均含结尾 NUL，长度与 CMSIS-DAP 约定一致。目标板固定信息（`TARGET_FIXED = 0`）仍返回 0。

### 6.5 状态指示灯

| 引脚 | 宏 | 语义 |
| --- | --- | --- |
| GPIO21 | `PROBE_DAP_CONNECTED_LED` | 连接灯：调试器已连接 |
| GPIO22 | `PROBE_DAP_RUNNING_LED` | 运行灯：目标处于运行状态 |

LED 为 `3V3 → 电阻 → LED → GPIO` 的灌电流接法，低电平点亮，`PROBE_LED_ACTIVE_LEVEL = 0`。

- `DAP_SETUP()` 在 `main()` 中把两脚配成输出，并先写熄灭电平再切方向，上电不会瞬亮。
- 两盏灯都只由 `DAP_HostStatus` 控制：`DAP_DEBUGGER_CONNECTED` 驱动连接灯，`DAP_TARGET_RUNNING` 驱动运行灯。USB 挂载/卸载回调不碰指示灯，上位机不下发时两灯保持熄灭。

两盏灯的实测方法见 `docs/test_led_dap_info.md`。

### 6.6 自定义 Vendor 命令（参考电压）

`module/dap/dap_vendor.c` 实现 `DAP_ProcessVendorCommand()`，用两条 Vendor 命令在线调整接口缓冲芯片的参考电压。电压以 0.1 V 为单位，`33` 表示 3.3 V。

| 命令 | 请求 | 说明 |
| --- | --- | --- |
| `0x80`（Vendor0） | `[0x80, value]` | 设置 VREF；`value` 取 0 或 12…33，并写入 EEPROM |
| `0x81`（Vendor1） | `[0x81]` | 读取当前设置 |

响应固定 5 字节：`[cmd, vref_10x, result, mv_lo, mv_hi]`。

- `vref_10x`：当前生效的电压（0.1 V 单位），关闭输出时为 0。
- `result`：`0` 成功，`1` 参数越界（未改动硬件），`2` IIC 访问失败（含器件不应答）。
- `mv_lo` / `mv_hi`：当前电压的毫伏值，小端；关闭输出时为 0。

规则：

- 只接受 12…33（1.2…3.3 V）与 0；其它值返回 `result = 1`，硬件保持原状。
- `0` 表示关闭 VREF 输出：DAC 进入掉电模式，下拉电阻由 `DAC_MCP4716_PD_RESISTOR` 选择（默认 3 = 640 kΩ 对地），并写入 EEPROM。
- 设置会通过命令 3 同时更新易失存储器和 EEPROM，约 50 ms 后返回；上电时 MCP4716 自动从 EEPROM 恢复该设置。EEPROM 寿命有限，不要高频重复设置。
- 3.3 V 受 10 位满量程限制，实际输出约 3.297 V（见 §9.6），因此轮询返回的毫伏值约为 3297。

OpenOCD 用法：

```text
cmsis-dap cmd 0x80 0x21   # 输出 3.3 V
cmsis-dap cmd 0x80 0x19   # 输出 2.5 V
cmsis-dap cmd 0x81        # 读回当前设置
cmsis-dap cmd 0x80 0x00   # 关闭 VREF 输出
```

OpenOCD 打印响应的第 1…4 字节，即 `vref_10x result mv_lo mv_hi`。实测方法见 `docs/test_vref_cmd.md`。

### 6.7 端点统计（Vendor 命令 0x82）

`module/dap/dap_stats.h` 声明统计接口，`tusb_edpt_handler.c` 在收发回调里维护计数，`dap_vendor.c` 把结果通过 `0x82`（`ID_DAP_Vendor2`）暴露给上位机，用于判断吞吐瓶颈在主机侧还是设备侧。`config/DAP_config.h` 的 `DAP_STATS_ENABLE` 控制是否编译，默认关闭；关闭时 `0x82` 不进入统计分支，按未知命令返回 `ID_DAP_Invalid`。

请求为 `[0x82, sel]`，响应固定 5 字节 `[cmd, b0, b1, b2, b3]`，OpenOCD 打印后 4 字节。

| `sel` | 用途 | 4 个数据字节 |
| --- | --- | --- |
| `0` | 环形深度 | 请求深度峰值、响应深度峰值、当前请求深度、当前响应深度 |
| `1` | OUT 包计数 | `out_packets`，小端 32 位 |
| `2` | IN 包计数 | `in_packets`，小端 32 位 |
| `3` | 毫秒计数 | 1 ms tick 计数，小端 32 位 |
| `4` | 清零 | 全 0 |
| `5` | 命令总耗时 | `busy_us`，小端 32 位 |
| `6` | 单条命令最长耗时 | 微秒，小端 32 位 |
| `7` | 请求最小间隔 | 微秒，小端 32 位；无数据时为 0 |
| `8` | 请求最大间隔 | 微秒，小端 32 位 |
| `9` | 实际 SWCLK | kHz，小端 32 位 |

用法：

```text
cmsis-dap cmd 0x82 4   # 清零
                       # 执行一次扫描
cmsis-dap cmd 0x82 0   # 深度峰值
cmsis-dap cmd 0x82 2   # IN 包数
cmsis-dap cmd 0x82 3   # 毫秒计数
cmsis-dap cmd 0x82 5   # 命令总耗时（微秒）
```

判读：

- 响应深度峰值反映主机允许多少个未读响应堆积；OpenOCD v2 后端实际只保持一个包在途，峰值通常为 1（原因见 `docs/openocd_pipeline.md`）。
- 请求深度峰值是请求环内同时排队的包数；DAP 线程处理很快时这个值通常也只有 1…2，不能单独用来判断主机是否流水线。
- `in_packets` 除以毫秒差即每秒响应包数，乘以每包有效载荷（OpenOCD 读内存时约 48 字节）可估算吞吐，用来与实测扫描速度对照。
- `busy_us` 除以包数得到设备处理单条命令的平均时间；它占整个传输时间的比例就是设备占用率，用来判断瓶颈在设备还是总线。
- 请求间隔的最小值反映主机成串发包的能力，最大值包含扫描开始/结束的间歇。
- `swclk_khz` 是 PIO 分频后实际生效的 SWCLK，用来核对主机的 `adapter speed` 是否真正生效。
- 统计命令自身也计入包数；每次 USB 配置时计数清零。

实测方法见 `docs/test_stats_cmd.md`。

## 7. 目标接口引擎（PIO）

### 7.1 `probe_oen.pio`

`driver/swd/probe_oen.pio` 是唯一的位流程序：**SWCLK 与 DOE（输出使能）** 作为侧集引脚、SWDI/SWDO 分离，用一根 DOE 控制外部缓冲芯片的方向，避免 SWDIO 引脚反复切换方向带来的时序抖动。

程序内共 27 条指令，同时提供 SWD 与 JTAG 的命令入口（地址表见 `docs/jtag_pio_design.md` §3.1）。SWD 与 JTAG 的引脚绑定差异（`in_base`/`out_base`/`set_base`）由 `probe_enter_swd()` / `probe_enter_jtag()` 在切换模式时改写 `PINCTRL`，程序本身不变。

### 7.2 PIO 命令格式

每个 TX FIFO 条目要么是命令，要么是紧随命令的数据字。命令字布局（对应 `probe.c` 的 `fmt_probe_command()`）：

| 位域 | 含义 |
| --- | --- |
| 31:12 | 短命令内联的数据；长写命令的数据由第二个 FIFO 条目提供；TMS 序列命令里是 4 个入口地址字段 |
| 11:7 | 命令入口地址（`probe_offset_*` 加程序基址） |
| 6:0 | 位计数减一（`bit_count - 1`），最大 128 位 |

程序中的命令入口：

| 入口 | 作用 |
| --- | --- |
| `write_cmd` | 先 `pull` 一个 32 位数据，再移位输出 `bit_count` 位（超过 32 位补 0） |
| `short_write_cmd` | 命令字 [31:12] 携带数据，位计数 ≤ 20 时把命令与数据合并成一次 FIFO 写入 |
| `turnaround_cmd` | 仅产生 `bit_count` 个高阻时钟（TURNAROUND） |
| `read_cmd` | 采样 `bit_count` 位并 `push` 到 RX FIFO |
| `get_next_cmd` | 命令分发主体：`pull` 命令字 → 取位计数 → `out pc, 5` 跳转 |
| `jtag_tms_lo_cmd` / `jtag_tms_hi_cmd` | 输出 TMS 电平，不产生时钟（TMS 与 SWD_DO 复用同一引脚） |
| `jtag_tms_seq_lo_cmd` / `jtag_tms_seq_hi_cmd` | 输出 1 拍 TMS 电平并产生 1 个 TCK，随后由 `out pc, 5` 从命令字里取下一拍的入口 |
| `jtag_rw_cmd` / `jtag_rw_short_cmd` | 走 `bit_count` 个 TCK，同拍输出 TDI 并采样 TDO；DOE 全程为 1 |

位计数字段只有 7 位，单条写命令最多 128 位；`probe_write_bits()` 的数据字只有 32 位，超过 32 位的部分输出 0。`probe_hiz_clocks()` 超过 20 位时按 20 位分片。

TMS 序列命令复用命令字的数据段：位计数段不参与，`[11:7]` 与 `[31:12]` 拆成 5 个 5 位字段，前 4 个字段是各拍的入口（`jtag_tms_seq_lo_cmd` / `jtag_tms_seq_hi_cmd`），最后一个字段固定填 `get_next_cmd`。因此一条 FIFO 写入即可产生 1～4 拍带时钟的 TMS 序列；`probe.c` 的 `jtag_tms_seq()` 只处理 4 拍以内，更长的序列由调用方拆分。

命令字与数据在 PIO 侧按 FIFO 顺序执行，因此写操作可以在投递后立即返回，无需等待完成；`probe_wait_idle()` 以「TX FIFO 为空 + PC 停在 `get_next_cmd`」为判据，在需要时等待整条流水线排空（模式切换依赖此机制保证命令已生效）。

`probe_put()` / `probe_get()` / `probe_wait_idle()` 都用 `time_us_64()` 做超时，上限由当前分频推出：一条命令等待的最坏情况是整条队列排空（4 条 128 位命令 = 2048 个 SM 周期），单 SM 周期 = `divider / clk_sys`，再乘 2 倍余量，结果钳在 1 ms～500 ms；10 kHz 下限对应 102.4 ms。超时后进入故障态：停止 SM、清空两个 FIFO、用 `pio_sm_exec()` 把 PC 拨回 `get_next_cmd`、重新使能，并置粘滞标志。故障期间 `probe_put()` 丢弃命令、`probe_get()` 直接返回 0，避免命令流错位；标志由 `probe_take_timeout()` 取走，`SWD_Transfer()` / `JTAG_Transfer()` 取到后返回 `DAP_TRANSFER_ERROR`，不再把未时钟出的数据当成正常数据交给上位机。

### 7.3 引脚与 SM 初始化

`probe_oen.pio` 内嵌的 `% c-sdk { }` 块提供两个静态内联函数，由 `probe_init()` 调用：

- `probe_gpio_init(pio)`：把 6 根信号脚交给 PIO；SWD_DO 上拉以维持空闲高电平；对 SWD_DI 与 JTAG_TDO 置位 `input_sync_bypass` 以减小输入延迟；RESET/TRST 上拉后设为输出以模拟开漏/推挽。
- `probe_sm_init(pio, sm_config)`：配置侧集（SWCLK + DOE）、输入引脚（SWD_DI）、输出引脚（SWD_DO，1 位）、set 引脚（SWD_DO，1 位，JTAG 下作 TMS）、移位方向（输入输出均右移、关闭自动 push/pull），并把各引脚初始方向设为 TDI 输入、TDO 输入、CLK 输出、DOE 输出、DI 输入、DO 输出。

`probe_init()` 只在首次调用时装载程序、配置并启用 SM，随后用 `pio_sm_exec()` 让 SM 跳转到 `get_next_cmd`，最后按 SWD 模式建立引脚绑定；`probe.initted` 标志保证幂等。

命令分发靠 `out pc, 5` 写入**绝对**程序地址，该地址不在程序镜像里、由命令字在运行时提供，因此 `fmt_probe_command()` 要把 `probe.offset` 加进入口地址——这样它与 SDK 装载时对 `jmp` 做的重定位保持一致，程序可以落在任意合法偏移上。约束是「偏移 + 最大命令入口 < 32」，由 `probe_init()` 的断言校验（见 `docs/jtag_pio_design.md` §3.3）。

### 7.4 模式切换

SWD 与 JTAG 共用同一个状态机与 PIO 程序，两者的差异只在引脚绑定与方向：

| 配置 | SWD | JTAG |
| --- | --- | --- |
| `IN_BASE` | `PROBE_SWD_DI_PIN`(5) | `PROBE_JTAG_TDO_PIN`(2) |
| `OUT_BASE` / `OUT_COUNT` | `PROBE_SWD_DO_PIN`(6) / 1 | `PROBE_JTAG_TDI_PIN`(0) / 1 |
| `SET_BASE` / `SET_COUNT` | 6 / 1（不使用） | 6 / 1（TMS） |
| PIO 侧 TDI / TDO 方向 | 输入 / 输入 | 输出 / 输入 |
| GPIO0 功能 | UART0 TX（CDC 桥） | PIO 输出（TDI） |
| GPIO12 功能 | 高阻输入 | UART0 TX（CDC 桥） |
| GPIO8（缓冲器方向） | 低（输入方向） | 高（输出方向） |

`probe_enter_jtag()` / `probe_enter_swd()` 先 `probe_wait_idle()`，再在 SM 停止的窗口内（`probe_sm_stop()` 与 `probe_sm_start()` 之间）改写 `PINCTRL` 与引脚方向，并调用 `cdc_uart_set_tx_pin()` 切换 CDC 桥 TX 引脚。切换后 TMS 的实际电平未知，JTAG 侧用 `jtag_tms_valid` 标记强制下一条 JTAG 操作重发 TMS。JTAG 模式下命令之间的取指窗口仍会让 `pull side 0x0` 关闭 TMS 的输出使能，见 §11.2。

### 7.5 SWD 事务流程

`SWD_Transfer()`（`module/dap/sw_dp_pio.c`）按 CMSIS-DAP 语义组织位流：

1. 组装 8 位请求包 `[Start=1][APnDP][RnW][A2][A3][Parity][Stop=0][Park=1]` 并写入。
2. 读取 `turnaround + 3` 位并右移取出 3 位 ACK。
3. ACK 为 OK 时：
   - 读事务：读 32 位数据 + 1 位奇偶校验，用 `__builtin_popcount` 校验；末尾补 `turnaround` 个高阻时钟。
   - 写事务：先补 `turnaround` 个高阻时钟，再写 32 位数据 + 1 位奇偶校验。
   - `IDLE_CYCLES` 非零时驱动相应数量的 0 时钟。
4. ACK 为 WAIT/FAULT 时，若 `data_phase` 使能则按方向消费或补发一个虚假数据帧。
5. 其他 ACK 视为协议错误，回退读取 `turnaround + 33` 位。

`idle_cycles` 超过 128 时按 128 位分片发送。

### 7.6 JTAG 事务流程

`module/dap/jtag_dp_pio.c` 替换上游 `JTAG_DP.c`，实现 `JTAG_Sequence()`、`JTAG_IR()`、`JTAG_ReadIDCode()`、`JTAG_WriteAbort()`、`JTAG_Transfer()` 五个函数，并额外提供 `JTAG_SWJ_Sequence()` 供 `SWJ_Sequence()` 在 JTAG 模式下转调。

- **位原语**：`probe.c` 导出 `jtag_tms()`（只改 TMS 电平）、`jtag_tms_seq()`（带时钟的 TMS 序列）与 `jtag_bits()`（走 n 个 TCK，同拍输出 TDI、采样 TDO）。
- **TMS 序列**：`jtag_tms_seq(n, tms)` 把不超过 4 拍的 TMS 电平序列打包成一条命令，每拍 4 个 SM 周期（2 拍低 + 2 拍高），与位循环的时钟周期一致。导航（`jtag_nav()` 的 1,0,0 / 1,1,0,0）与收尾的 Update/Idle（1,0）都由它一次完成；`JTAG_SWJ_Sequence()` 的 ≤8 拍等值段按 4 拍一组走这条路径，更长的等值段用 `jtag_clocks()`（一条 TMS 命令 + 按 128 拍分片的批量时钟）。该命令不驱动 TDI，只用于 TDI 无关的 TAP 状态。
- **JTAG_Sequence**：TMS 在一次调用内恒定，`JTAG_Sequence()` 把 ≤64 位的 TDI 字节流装成 1～2 个 32 位字后调用 `jtag_bits()`，捕获结果再按 LSB first 写回字节缓冲。
- **JTAG_IR / JTAG_ReadIDCode / JTAG_WriteAbort / JTAG_Transfer**：按上游的 TAP 状态机路径逐段映射，导航与 Idle 用 `jtag_tms_seq()`，bypass 段用 TDI=1（IR 前）或 TDI=0 填充。
- **数据段**：读事务用 `jtag_bits(31, ...)` 批量采回 D0..D30；写事务同样用一条 `CMD_WRITE` 批量输出 D0..D30，只有 D31 与 Exit1-DR 同拍单独下发。事务末尾的 `idle_cycles` 由 `jtag_clocks()` 发送，超过 128 拍分片。
- **SWJ_Sequence**：JTAG 模式下数据是逐位 TMS 电平流，按「等值段」拆成多次扫描，段内 TDI 不关心。
- 时钟换算与 SWD 一致：`DAP_SWJ_Clock` 直接写 PIO 分频，SWD/JTAG 各操作入口不再重复处理时钟。
- 多器件链（`ir_before`/`ir_after` 非 0）按上游语义实现；实测覆盖的是单器件链（`count = 1`、`index = 0`）。

详细的指令地址表、逐拍时序与实现注意事项见 `docs/jtag_pio_design.md`，实测方法与判读见 `docs/jtag_diag_plan.md`。

## 8. CDC 串口桥

`driver/uart/cdc_uart_v2.c` 实现 USB-CDC 与 UART0 之间的双向桥接，默认 `DMA_OR_IRQ = 1`（DMA 方案）。

- **缓冲**：`rx_ringbuf`、`tx_ringbuf` 各 1024 字节，通过 `RINGBUF_STATIC_ALLOC` 静态分配（不使用 malloc）。
- **TX（USB → UART）**：`cdc_task()` 把 CDC 数据写入 `tx_ringbuf` 并触发 TX DMA；DMA 完成后在 `dma_irq0_handler()` 中 `ringbuf_consume()` 并按需续传。
- **RX（UART → USB）**：RX DMA 以环形方式持续接收，`cdc_task()` 周期性读取 `transfer_count` 差值把已收数据计入 `rx_ringbuf`，再写入 CDC IN 端点。
- **阻塞保护**：Windows CDC-ACM 驱动同时只提交一个 URB，若 CDC 写缓冲连续 20 次不可写，则跳过该轮的 CDC 输出，避免阻塞 GUI 方向的日志。
- **波特率与格式**：`tud_cdc_line_coding_cb()` 接收上位机设置，按位率换算轮询间隔与 LED 去抖，然后置 `uart_resetting` 由 UART 线程重新初始化 UART（含数据位、停止位、校验）。1.5 停止位按 2 位处理。
- **DMA 掩蔽**：任务与中断同时访问环形缓冲与 DMA 计数，临界区通过 `irq_set_enabled(DMA_IRQ_0, ...)` 关闭/打开 DMA 中断实现。
- **TX 引脚**：CDC 桥的 UART0 TX 随调试模式切换，由 `cdc_uart_set_tx_pin()` 配置。SWD 模式下 TX 在 GPIO0（`PROBE_UART_TX_SWD_PIN`）；JTAG 模式下 GPIO0 归 PIO 作 TDI，TX 改到 GPIO12（`PROBE_UART_TX_JTAG_PIN`），并由 GPIO8（`PROBE_UART_TX_OE_PIN`）拉高使能该脚外部缓冲器的输出方向。引脚变化时按 CDC 线程的复位流程重建串口：中止 DMA、关闭串口、清空环形缓冲、重配引脚、重开串口并重启 DMA 与中断；多核配置下先读当前任务亲和性，未限定在核 0 时切到核 0 打开中断，退出前恢复原亲和性，保证中断与 CDC 线程落在同一核心。

## 9. IIC 总线与参考电压生成

### 9.1 用途

IIC 总线上挂有一片 MCP4716A0，用于为接口缓冲芯片提供参考电压（VREF），从而让调试接口的 IO 电平可跟随目标板电压域。缓冲芯片由 4 V 供电，MCP4716A0 的 VREF 引脚接 3.3 V。

### 9.2 总线层

`driver/iic_shard/` 提供 IIC1（400 kHz）的互斥访问：

- `iic_shard_init()` 初始化 I2C1、配置 SDA/SCL 引脚与上拉，并创建优先级继承互斥量（`xSemaphoreCreateMutex`）。已在 `start_task()` 中调用。
- `iic_shard_lock()` / `iic_shard_unlock()` 带 1000 ms 超时（`IIC_SHARD_LOCK_TIMEOUT_MS`），超时返回失败而不是永久阻塞。
- `iic_shard_write_block()` / `iic_shard_read_block()` 直接转发到 Pico SDK 的 `i2c_*_blocking()`，**不带锁**，调用方负责加锁。

### 9.3 器件要点

| 项目 | 取值 |
| --- | --- |
| 器件 | Microchip MCP4716A0，单通道 10 位电压输出型 DAC，带 EEPROM |
| 分辨率 | 10 位（0…1023） |
| 接口 | I2C，400 kHz，器件不自拉伸时钟 |
| 从机地址 | `1100000b` = `0x60`（器件码 `1100` + 地址位 `A2:A0 = 000`） |
| 参考电压 | `VREF = 3.3 V`（外部，缓冲模式） |
| 供电 | 4 V |
| 输出电压 | `VOUT = VRL × D / 1024 × Gain`，`VRL` 为电阻梯参考电压 |

### 9.4 写命令与读命令

器件提供三条写命令与一条读命令，**只有命令 3 会更新 EEPROM**；命令 1/2 只修改易失存储器，掉电后丢失：

| 命令 | 字节数 | 首字节 | 数据字节 | 更新的内容 |
| --- | --- | --- | --- | --- |
| 1：写易失 DAC 寄存器 | 2 | `[0 0][PD1 PD0][b11 b10 b09 b08]` | 字节 2 = `b07:b00` | DAC 码值 + 掉电位 |
| 2：写易失存储器 | 3 | `[0 1 0][REF1 REF0][PD1 PD0][GAIN]` | 字节 2:3 = `b15:b00` | 易失配置位 + DAC 码值 |
| 3：写全部存储器 | 3 | 同命令 2，指令改为 `0 1 1` | 字节 2:3 = `b15:b00` | 易失配置位 + DAC 码值 + EEPROM |
| 读 | — | 地址字节 + R/W=1 | 器件返回 6 字节 | 易失/非易失状态、配置位与码值 |

两个数据字段的用法：

- 命令 1 的数据段为 12 位：字节 1 低 4 位 = `b11:b08`，字节 2 = `b07:b00`。MCP4716 的 10 位码值位于 `b11:b02`，即

  ```
  byte1[3:0] = (code >> 6) & 0x0F     // 码值 b09:b06
  byte2      = (code << 2) & 0xFF     // 码值 b01:b00 落在 byte2 的 bit7:6
  ```

- 命令 2/3 的数据段为 16 位（字节 2:3），MCP4716 占其高 10 位 `b15:b06`，低 6 位为无关位，即码值左移 6 位填入。

两种对齐方式实质相同：都是把码值放到数据字段的最高 10 位。因此输出与码值的关系为 `VOUT = VREF × code / 1024 × GAIN`，代码 1024 对应满量程（10 位码值上限 1023）。

读命令返回 6 字节：`[易失状态/配置][易失码值高][易失码值低][非易失状态/配置][非易失码值高][非易失码值低]`。状态字节的 bit7 为 RDY/BSY，bit6 为 POR，bit5 区分易失（0）/非易失（1），bit4:3 为 `REF1:REF0`，bit2:1 为 `PD1:PD0`，bit0 为 `GAIN`；码值同样位于 16 位数据字段的高 10 位。

关键配置位取值：

| 字段 | 取值 | 含义 |
| --- | --- | --- |
| `REF1:REF0` | `0x` | VDD（未缓冲） |
| `REF1:REF0` | `10` | VREF 引脚（未缓冲） |
| `REF1:REF0` | `11` | VREF 引脚（缓冲）——本板使用 |
| `PD1:PD0` | `00` | 正常输出 |
| `PD1:PD0` | `01`/`10`/`11` | 掉电，VOUT 接 1 k / 125 k / 640 k 对地 |
| `GAIN` | `0`/`1` | 增益 1 倍 / 2 倍（VDD 为参考时 `GAIN` 被忽略） |

**器件上电时自动把 EEPROM 中的码值与配置位装入易失存储器**，因此初始化只读回状态，不应写寄存器去保持默认输出。驱动在设置电压时通过命令 2（不保存）或命令 3（保存）一并写入配置位，确保参考源为 VREF 缓冲、增益 1 倍。出厂 EEPROM 为 VDD 参考、码值 0、正常输出，所以未配置过的板子上电输出接近 0 V；首次通过 Vendor 命令设置电压后才会写入正确的参考配置并持久保存。

### 9.5 驱动实现

`driver/dac_mcp4716/` 实现器件驱动，全部总线访问包裹在 `iic_shard_lock()` / `iic_shard_unlock()` 内：

| 接口 | 使用的命令 | 说明 |
| --- | --- | --- |
| `dac_mcp4716_init()` | 读 | 重试读取 6 字节（覆盖上电复位与 EEPROM 写周期，最长 30 × 2 ms），从易失状态恢复码值与掉电状态；不写任何寄存器，重复调用直接返回成功。 |
| `dac_mcp4716_set_mv(mv, save_to_eeprom)` | 命令 2 / 命令 3 | 按目标毫伏换算码值；`mv = 0` 进入掉电模式，下拉电阻由 `DAC_MCP4716_PD_RESISTOR` 选择。`save_to_eeprom` 为 true 时用命令 3 写入 EEPROM 并等待约 50 ms，否则用命令 2 只写易失存储器。码值、掉电状态（以及保存时的 EEPROM 状态）均未变化时跳过总线访问。 |
| `dac_mcp4716_get_mv()` | — | 读取缓存的当前电压；掉电或未初始化时返回 0。 |

码值换算使用整数定点运算（无浮点），并在换算时钳位到满量程：

```c
code = (mv * 1024 + VREF_MV * GAIN / 2) / (VREF_MV * GAIN);   /* 四舍五入 */
if (code > 1023) code = 1023;                                  /* 满量程钳位 */
```

板级参数集中在 `config/dap_board.h`：

| 宏 | 取值 | 说明 |
| --- | --- | --- |
| `DAC_MCP4716_I2C_ADDR` | `0x60` | 7 位从机地址 |
| `DAC_MCP4716_VREF_MV` | `3300` | VREF 引脚电压 |
| `DAC_MCP4716_PD_RESISTOR` | `3` | 掉电下拉：1 = 1 kΩ，2 = 125 kΩ，3 = 640 kΩ |

参考电压源（`DAC_MCP4716_VREF_SEL`，默认 2 = VREF 缓冲）与增益（`DAC_MCP4716_GAIN`，默认 1 倍）在 `driver/dac_mcp4716/mcp4716.h` 中给出缺省值，可由板级覆盖。

运行时的在线调整通过 §6.6 的 Vendor 命令完成。

### 9.6 持久化与精度

初始化不写任何寄存器：MCP4716 上电时自动从 EEPROM 加载码值与配置位。出厂 EEPROM 的码值为 0、参考为 VDD、正常输出，因此未配置过的板子上电 VOUT 接近 0 V。通过 Vendor 命令设置一次电压后，配置位和码值写入 EEPROM，之后每次上电都会恢复该设置；写入 0 则保存掉电状态。

保存 3300 mV 时，按 10 位分辨率与 3.3 V 参考换算得到码值 1023（满量程），实际输出为：

```
VOUT = 3300 mV × 1023 / 1024 ≈ 3296.8 mV
```

与目标值相差约 3.2 mV（不足 1 LSB）。这是满量程码值本身对应 `(2^n - 1) / 2^n` 的固有关系，不是配置误差；MCP4716 在码值 0x3FF 处的满量程误差本身为 -1.1 LSB（典型），因此实际偏差会略大于该理论值。`dac_mcp4716_get_mv()` 返回由码值反算的毫伏值，因此 3.3 V 档读回约 3297 mV。

`VREF = 3.3 V` 处于缓冲模式的允许输入范围（`0.01 V … VDD - 0.04 V` = 0.01…3.96 V）内，输出满量程 3.2968 V 也远低于 4 V 供电下的 `VOUT(max) = VDD - 0.04 V`，因此不存在裕量问题。

## 10. 未完成工作

### 10.1 SWO 跟踪

**现状**：`SWO.c` 已随上游编译，但 `SWO_UART`、`SWO_MANCHESTER`、`SWO_STREAM` 均为 0，`DAP_SWO_*` 命令一律返回不支持，`SWO_*` 回调也没有对应的硬件实现。

**实现要点**：

1. 物理通道复用 JTAG TDO 输入（GPIO2）。SWD 模式下 TDO 本来就归 PIO 作输入，SWO 从同一根线取数据，不需要额外引脚。
2. 需要一份独立的 PIO 程序解释输入位流：异步 UART 模式按 `DAP_SWO_Baudrate` 设定分频后过采样并在位中央判决；曼彻斯特模式需要先做跳变沿解码。
3. 收到的字节流写入 `SWO_BUFFER_SIZE`（4 KB）环形缓冲，由 `DAP_SWO_Data` 取走；同时提供 `DAP_SWO_Status` / `DAP_SWO_ExtendedStatus` 的上溢、空闲位计数。
4. SWO 与调试时序共用同一个状态机，采集期间无法并发执行 SWD/JTAG 事务，需要在 `SWO_Control` 启停与 `probe` 的状态之间做仲裁。

该部分工作量较大，作为独立任务处理。

### 10.2 其他待办

| 项目 | 说明 |
| --- | --- |
| `usbd_control_xfer_cb` 断言 | 见 §11.1，上游 TinyUSB 0.18.0 的 RP2040 DCD 总线复位竞态；本仓库不修复，待同步上游版本 |
| 主机测试 | 目前无可运行的 cmock 主机测试 |

`probe_deinit()` 仍为空实现：接口缓冲芯片的输入侧不允许悬空，断开连接时保持现有电平比把 IO 切成高阻更安全，因此 `PORT_OFF()` 不做引脚回收。

## 11. 已知问题

### 11.1 `usbd_control_xfer_cb` 断言失败

**现象**：设备串口日志出现

```
usbd_control_xfer_cb 157: ASSERT FAILED
```

**根因**：上游 TinyUSB 0.18.0（Pico SDK 2.2.0）的 RP2040 DCD 在总线复位时只清 `dev_addr_ctrl` 与 EP1–EP15，不复位 EP0；TinyUSB 随后执行 `usbd_reset()` → `usbd_control_reset()`，把当前控制请求清零。若复位前在途的 EP0 IN 数据阶段完成事件在复位事件之后才入队，`usbd_control_xfer_cb()` 按「方向与当前请求相反」将其判为状态阶段，并因 `xferred_bytes != 0` 触发 `TU_ASSERT`。

**触发条件**：枚举期间主机发生 USB 总线复位，且复位与 EP0 数据阶段完成重叠。属于时序竞态，不是每次必现，与 JTAG/SWD 模式及是否连接目标无关。

**影响**：`TU_ASSERT` 返回 `false`，该完成事件被丢弃；总线复位已清空控制状态，重新枚举通常不受影响。风险是迟到的完成事件在新 SETUP 之后被误当成新请求的数据阶段。

**状态**：上游已知竞态，本仓库不修复。Pico SDK 2.2.0 携带的 TinyUSB 0.18.0 仍有此问题；同步到包含该修复的上游版本（控制传输按 SETUP 标签区分、总线复位单独通知）后消除。上游同类记录见 [tinyusb#2322](https://github.com/hathach/tinyusb/issues/2322)。

### 11.2 JTAG 模式下取指等待窗口关闭 TMS 输出使能

**现象**：JTAG 模式下状态机每次回到 `get_next_cmd` 执行 `pull side 0x0`，侧集把 DOE 驱动为 0，外部缓冲器关断输出，与 SWD_DO 复用同一引脚的 TMS 在两条命令之间进入高阻，直到下一条命令重新把 DOE 置 1。等待时间越长，TMS 无驱动的窗口越长。

**根因**：`get_next_cmd` 的 `pull side 0x0` 同时设定 SWD 的空闲电平（SWCLK=0、DOE=0），在 SWD 模式下令缓冲器高阻是正确行为。JTAG 模式下 TMS 与 SWD_DO 是同一根引脚，要求常驱动；所有 JTAG 命令逐条以侧集 0x2/0x3 打开 DOE，只在命令执行期间满足该要求，命令之间的取指等待窗口仍回到 0x0。

**影响**：取指窗口内 TMS 不再被主动驱动，线上电平由外部上拉与目标侧决定，理论上可能不稳定。该窗口内没有 TCK 边沿（TCK 保持低），而每个时钟边沿所在的命令都已把 DOE 置 1，TAP 只在 TCK 上升沿采样 TMS，因此实际未观察到影响。需求上依然不符合「TMS 常输出」。

**候选方案（未采用）**：在 `probe_enter_jtag()` / `probe_enter_swd()` 已有的 SM 停机窗口内改写指令存储器中 `get_next_cmd` 的 `pull` 指令，JTAG 模式把侧集换成 0x2，切回 SWD 时写回 0x0：

```c
/* enter JTAG */
pio->instr_mem[probe.offset + 6] = pio_encode_pull(false, true) | pio_encode_sideset_opt(2, 0x2);

/* enter SWD: restore the idle sideset */
pio->instr_mem[probe.offset + 6] = pio_encode_pull(false, true) | pio_encode_sideset_opt(2, 0x0);
```

0x2 对应「侧集生效、SWCLK=0、DOE=1」，即 JTAG 命令之间的空闲侧集；0x0 的编码与 PIO 当前装载的 `pull` 指令（`0x90a0`）完全一致，可以原样还原。缺点是硬编码了指令偏移 6，`probe_oen.pio` 一旦增删指令该偏移就会变，必须重新核对 `probe_offset_get_next_cmd`；用该宏代替常量可以避免行号失配。更彻底的做法是在 `probe_oen.pio` 中让取指入口按模式区分，而不是运行时改写指令。

**状态**：仅记录，代码未修改。


