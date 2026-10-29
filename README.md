# daplink

基于 RP2350A 的 CMSIS-DAP 兼容调试器固件。调试接口时序由 PIO 生成，系统运行 FreeRTOS（SMP，双核），USB 设备栈使用 TinyUSB。

详细设计见 [docs/design.md](docs/design.md)。

## 功能

- CMSIS-DAP v2（WinUSB Bulk 接口，含 MS OS 2.0 描述符，Windows 免驱）
- SWD 调试通道，PIO 生成时序，时钟频率由上位机通过 `DAP_SWJ_Clock` 设置
- JTAG 调试通道，与 SWD 共用 PIO 状态机；TDI/TDO/TCK/TMS 由 PIO 驱动，nTRST 为独立 GPIO
- `DAP_Info` 返回厂商、产品、序列号与固件版本字符串（序列号与 USB 描述符同源）
- 状态指示灯：连接灯 GPIO21、运行灯 GPIO22，低电平点亮，由 `DAP_HostStatus` 驱动
- USB-CDC 转 UART 桥（UART0，DMA + 环形缓冲；TX 随调试模式切换：SWD 用 GPIO0，JTAG 用 GPIO12）
- IIC1 总线上的 MCP4716A0 参考电压 DAC，可由 Vendor 命令设置或关闭并写入 EEPROM，上电自动恢复
- 1 s 看门狗，双核心跳；看门狗复位后进入 USB Bootloader

## 未完成

- SWO 跟踪：复用 TDO 输入、需要独立的 PIO 解码程序，作为单独任务实现，见 [docs/design.md](docs/design.md) §10.1

## 构建

依赖 Pico SDK 2.x、`arm-none-eabi-gcc` 与 `git`，构建目录固定为仓库根目录下的 `build/`。`git` 用于把 `patches/` 下的 `DAP.c` 补丁应用到 `build/generated/`，原始子模块源码保持不变。

**不要**启用 `PICO_SDK_FETCH_FROM_GIT`，直接指向已检出的 SDK。

macOS：

```sh
export PICO_SDK_PATH=/path/to/pico_sdk-src
export PICOTOOL_FETCH_FROM_GIT_PATH=/path/to/pico_sdk_fetch
mkdir -p build
cmake -S . -B build
cmake --build build -j8
```

Windows（picotool 使用仓库内 `tools/picotool`，由 `CMakeLists.txt` 指定）：

```bat
set PICO_SDK_PATH=D:/code/pico-sdk
if not exist build mkdir build
cmake -S . -B build
cmake --build build -j8
```

产物为 `build/daplink.elf/.bin/.hex/.uf2`，链接结束会打印 FLASH/RAM 占用。

在 `driver/` 下新增目录或 `.c` 文件后必须重新执行 `cmake -S . -B build`：`driver/CMakeLists.txt` 使用不带 `CONFIGURE_DEPENDS` 的 `file(GLOB driver/*/*.c)` 收集源文件。

## 目录结构

| 路径 | 说明 |
| --- | --- |
| `app/` | 程序入口、启动任务、看门狗 |
| `bsp/` | Pico SDK 引入 |
| `config/` | 板级定义、CMSIS-DAP 适配层、FreeRTOS 与 TinyUSB 配置 |
| `driver/` | 底层驱动：`swd`（PIO 位流）、`iic_shard`（IIC 总线）、`dac_mcp4716`（参考电压 DAC）、`uart`（CDC 桥） |
| `lib/` | CMSIS-DAP Firmware、CMSIS_6、FreeRTOS-Kernel |
| `module/` | USB 接入、DAP 端点处理、日志、工具 |
| `tools/` | Windows 下预置的 picotool |
| `patches/` | 构建时应用到上游源码副本的补丁与补丁脚本 |
| `docs/` | 设计文档与测试用例 |

## 测试

- MCP4716A0 参考电压驱动：[docs/test_mcp4716.md](docs/test_mcp4716.md)
- VREF Vendor 命令：[docs/test_vref_cmd.md](docs/test_vref_cmd.md)
- 端点统计 Vendor 命令：[docs/test_stats_cmd.md](docs/test_stats_cmd.md)
- OpenOCD v2 流水线缺陷与优化：[docs/openocd_pipeline.md](docs/openocd_pipeline.md)
- SWJ 时钟频率：[docs/test_swj_clock.md](docs/test_swj_clock.md)
- JTAG 实测步骤与判读：[docs/jtag_diag_plan.md](docs/jtag_diag_plan.md)
- 状态指示灯与 `DAP_Info` 字符串：[docs/test_led_dap_info.md](docs/test_led_dap_info.md)

## 许可

MIT，见各源文件头部声明；`lib/` 下的第三方代码遵循其各自许可。
