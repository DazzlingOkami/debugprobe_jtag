# SWJ 时钟频率测试

`DAP_SWJ_Clock` 的请求值直接写入 PIO 分频，无法用 cmock 在主机上覆盖，按下面的方法在实机上测量 SWCLK/TCK。

## 1. 测量方法

1. 逻辑分析仪接 `PROBE_SWD_CLK_PIN`（GPIO3，SWD 的 SWCLK 与 JTAG 的 TCK 共用）。
2. 上位机连接调试器后，用 `adapter speed <kHz>` 设置频率。OpenOCD 的 CMSIS-DAP 驱动会把它换算成 Hz 后下发 `DAP_SWJ_Clock`。
3. 发起连续访问（例如循环读同一寄存器，或执行 `dap info` / JTAG IDCODE 扫描），抓取至少包含 10 个完整时钟周期的波形。
4. 用相邻上升沿的时间间隔计算频率：`f = 1 / T`。取多段平均，测量误差应小于 0.1%。

示例（目标相关的访问命令按实际板卡调整）：

```powershell
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -c "adapter speed 18000" `
        -c "init" -c "dap info" -c "shutdown"
```

若 `adapter speed` 无法覆盖目标值，可用 `cmsis-dap cmd 0x11` 直接下发 `DAP_SWJ_Clock`：请求为 4 字节小端 Hz，例如 18 MHz = `0x0112A880`：

```powershell
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -c "init" `
        -c "cmsis-dap cmd 0x11 0x80 0xA8 0x12 0x01" -c "dap info" -c "shutdown"
```

## 2. 预期结果

`clk_sys = 150 MHz`，PIO 每个时钟位占 4 个 SM 周期，分频值为 `divider = 150 MHz / (4 × f)`，以 1/256 为单位向上取整，因此实际频率不大于请求值。

| 请求频率 | `adapter speed` | 分频（整数 + 1/256） | 预期 TCK | 相对误差 |
| --- | --- | --- | --- | --- |
| 1 MHz | 1000 | 37.500 | 1.0000 MHz | 0 |
| 10 MHz | 10000 | 3.750 | 10.0000 MHz | 0 |
| 12 MHz | 12000 | 3.125 | 12.0000 MHz | 0 |
| 16 MHz | 16000 | 2.34375 | 16.0000 MHz | 0 |
| 18 MHz | 18000 | 2.085938 | 17.9775 MHz | 0.13% |
| 19 MHz | 19000 | 1.976563 | 18.9723 MHz | 0.15% |
| 20 MHz | 20000 | 1.875 | 20.0000 MHz | 0 |
| 24.999 MHz | 24999 | 1.503906 | 24.9351 MHz | 0.26% |
| 25 MHz | 25000 | 1.500 | 25.0000 MHz | 0 |
| 37 MHz | 37000 | 1.015625 | 36.9231 MHz | 0.21% |
| 37.5 MHz | 37500 | 1.000 | 37.5000 MHz | 0 |
| 50 MHz | 50000 | 1.000 | 37.5000 MHz | 上限钳位 |
| 1 kHz | 1 | 3750.000 | 10.000 kHz | 下限钳位 |

## 3. 检查项

| 项目 | 方法 | 预期 |
| --- | --- | --- |
| 默认频率 | 上电枚举后不下发 `DAP_SWJ_Clock`，直接发起访问 | 1 MHz |
| 立即生效 | 在一次连接内连续设置 1 MHz、18 MHz、25 MHz，每次设置后立即抓波形 | 每次测得的频率与上表一致，不出现上一档的残留 |
| 模式切换 | SWD 与 JTAG 使用同一设定，切换 `transport select` 后重新访问 | 两种模式测得的频率相同，切换后不回到 1 MHz |
| 重连保持 | 设置 25 MHz 后断开再连接 | 仍为最近一次设置的频率 |
| 下限 | `adapter speed 1` | 10 kHz，且调试访问仍能完成 |
| 上限 | `adapter speed 50000` | 37.5 MHz |

若实测值落在请求值以下且偏差大于一个分频步进，先检查 `clock_get_hz(clk_sys)` 是否确实为 150 MHz；若频率完全不变，检查 `build/generated/lib/CMSIS-DAP/Firmware/Source/DAP.c` 是否包含 `probe_set_swclk_freq_hz`。
