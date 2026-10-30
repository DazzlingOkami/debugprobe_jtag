# 状态指示灯与 DAP_Info 字符串测试

两项功能都无法用 cmock 在主机上覆盖，按下面的方法在实机上验证。

## 1. DAP_Info 字符串

`DAP_Info` 的命令号为 `0x00`，请求为 `[0x00, id]`，响应为 `[0x00, len, data...]`，`len` 含结尾 NUL。

| id | 信息项 | 期望值 | len |
| --- | --- | --- | --- |
| `0x01` | Vendor | `Cassini` | 8 |
| `0x02` | Product | `CMSIS_DAP` | 10 |
| `0x03` | Serial Number | 16 位十六进制串，与 USB 描述符里的序列号相同 | 17 |
| `0x09` | Product Firmware Version | `2.10` | 5 |
| `0x05` / `0x06` | Device Vendor / Device Name | 空（`TARGET_FIXED = 0`） | 0 |
| `0x07` / `0x08` | Board Vendor / Board Name | 空（`TARGET_FIXED = 0`） | 0 |

用 OpenOCD 直接下发命令（`adapter serial` 换成实际序列号，可省略）：

```powershell
openocd -c "adapter driver cmsis-dap; adapter serial 5148E980C9C8E778; transport select swd" `
        -c "init" -c "cmsis-dap cmd 0x00 0x01" -c "cmsis-dap cmd 0x00 0x02" `
        -c "cmsis-dap cmd 0x00 0x03" -c "cmsis-dap cmd 0x00 0x09" -c "shutdown"
```

判读：返回数据里应能看到对应的 ASCII 字符串，且最后一字节为 `00`。例如 Vendor 的响应体为 `00 08 43 61 73 73 69 6E 69 00`。

同一组字符串在 Keil MDK、pyOCD 等上位机的调试器信息里也应显示出来；若显示为空，说明 `DAP_Info` 的响应没有被正确解析，此时先核对 `len` 是否等于上表的值。

## 2. 状态指示灯

### 2.1 USB 灯

| 步骤 | 预期 |
| --- | --- |
| 未上电 | 两灯均不亮 |
| 用仅供电的 USB 线连接主机（D+/D- 不通） | USB 灯亮，DAP 连接灯灭 |
| 换成数据线，主机枚举完成 | USB 灯灭，DAP 连接灯仍灭 |
| 拔掉 USB（整机断电） | 两灯均灭 |

USB 灯为红灯，指示的是“主机未配置设备”，与调试器是否连接目标无关。

### 2.2 用 DAP_HostStatus 控制 DAP 连接灯

`DAP_HostStatus` 命令号为 `0x01`，请求为 `[0x01, type, status]`，`type`：`0` = 调试器已连接，`1` = 目标运行中。

```powershell
openocd -c "adapter driver cmsis-dap; transport select swd" -c "init" `
        -c "cmsis-dap cmd 0x01 0x00 0x01" -c "sleep 1000" `
        -c "cmsis-dap cmd 0x01 0x00 0x00" -c "sleep 1000" -c "shutdown"
```

预期：DAP 连接灯亮 → 灭，两条命令之间间隔 1 s，便于观察。`type = 1`（目标运行中）不驱动任何灯，下发后灯的状态不变。

响应为 `[0x01, 0x00]`（命令号 + `DAP_OK`）。OpenOCD 固定打印响应中命令号之后的 4 字节，所以每次打印的都是 `Returned data 00 00 00 00`，不能据此判断命令是否生效，以灯的动作是否随之变化为准。

### 2.3 上位机联调

用 pyOCD 或 Keil 连接目标后，DAP 连接灯应点亮，断开调试会话后熄灭；halt / resume 不影响任何灯。若上位机不下发 `DAP_HostStatus`，连接灯会一直保持熄灭，属正常。`cmsis-dap cmd` 与上位机交替下发时，连接灯状态以最后一条 `DAP_HostStatus` 为准。

若灯的亮灭与上述预期相反，把 `config/dap_board.h` 里的 `PROBE_LED_ACTIVE_LEVEL` 由 `0` 改为 `1`（该板为低电平点亮）。
