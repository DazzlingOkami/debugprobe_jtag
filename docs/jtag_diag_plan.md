# JTAG 实测步骤与判读

本文档给出 JTAG 通路的实测方法与判读标准；时序实现见 `docs/jtag_pio_design.md`。

## 1. 用 OpenOCD 直接下发 JTAG 序列（不接目标板也能验证 `JTAG_Sequence`）

断开目标板，在 JTAG 连接器上把 **TDI 与 TDO 短接**，然后执行：

```powershell
openocd -c "adapter driver cmsis-dap; adapter serial 5148E980C9C8E778; transport select jtag" `
        -c "init" -c "cmsis-dap cmd 0x14 0x01 0x80 0xaa" -c "shutdown"
```

含义：`0x14` = `DAP_JTAG_Sequence`，序列数 1，序列信息 `0x80` = 捕获 TDO（8 位、TMS=0），TDI 数据 `0xaa`。

判读：

- 输出 `Returned data 20 02 aa 00` → `resp[3]=0xaa` 与送入的 TDI 相同，说明 TCK 在走、TDI 送到了连接器、TDO 也回到了 GPIO2；
- `resp[3]` 是 `00` 或 `ff` → 该通路有问题，数值指出是 TDI 没出去（`00`）还是 TDO 没回来（`ff`）。

## 2. 逻辑分析仪核对

接 TCK/TMS/TDI/TDO 四根线，用 TCK 上升沿触发，建议先把 TCK 降到 1 MHz 以下。

判读要点：

- **TCK 频率**：实测周期应等于 `probe_get_tck_freq()` 的返回值（按 `CPU_CLOCK` = 150 MHz 折算）；
- **TMS 在上升沿之前稳定**；每个 TMS 位都对应一拍 TCK（TAP 每次状态迁移都需要一拍）；
- **移位末位与 Exit1 同拍**：`Shift-DR` 的最后一位与进入 `Exit1-DR` 的那一拍是同一个时钟；
- **TDO 只在 Shift-IR / Shift-DR 由目标驱动**；若解码状态出现 `PAUSE-DR`，说明导航错了（`Exit1 → Update` 必须 `TMS=1`）；
- 从 `Test-Logic-Reset` 出来必须先用一拍 `TMS=0` 进 `Run-Test/Idle`，`TMS=1` 只会留在复位里。

## 3. 读 IDCODE 的正确姿势

**不要装载 IR**：TLR 之后 TAP 默认装载 IDCODE，直接走 `Idle → Select-DR-Scan → Capture-DR → Shift-DR` 读 32 位即可。硬编码 IR 长度（例如按 4 位移入 0x0E）在 IR 长度不符的链上会把指令装错，随后 DR 读回全 0（选中 BYPASS）。

需要走 IR 时，长度必须来自上位机 `DAP_JTAG_Configure` 下发的 `ir_length`/`ir_before`/`ir_after`。

## 4. 常见误判

| 现象 | 判读 |
| --- | --- |
| TDO 恒为 0 | 先查 nTRST 是否一直处于复位有效态，再查 TDO 通路；nTRST 有效时扫描同样读回全 0 |
| TDI↔TDO 环回正确但读不到目标 | 环回只验证探针自发自收，不覆盖目标侧驱动 |
| 读回值整体偏移一位 | 采样点或填充拍不对，按 `docs/jtag_pio_design.md` §4.1 增加补偿拍 |
| 解码出 `Pause-DR/IR` | TAP 导航错：`Exit1 → Update` 必须 `TMS=1`，`Update → Idle` 才是 `TMS=0` |
| DR 读回全 0 且刚装过 IR | IR 长度与链不符，指令被装到错误位置并选中 BYPASS；读 IDCODE 时不要装载 IR |
| 引脚方向改了不生效 | `pio_sm_set_pindirs_with_mask()` 靠注入 `set pindirs` 实现，只能在状态机停止时改 |

调试期间可以临时用 `printf` 观察，日志走 UART1（GPIO20，115200 8N1）；不要用上位机打印的响应布局反推固件内部行为，直接看固件侧状态更快。
