# JTAG 时序的 PIO 实现方案

本文档给出在 `driver/swd/probe_oen.pio` 与 `driver/swd/probe.c` 上实现 JTAG 时序、使 SWD 与 JTAG 共存于同一 PIO 状态机的完整方案，作为编码前的评审依据。SWD 通道已实机验证，本方案不改动其命令编码与时序。

## 1. 目标与约束

| 项目 | 结论 |
| --- | --- |
| 复用范围 | 同一个 PIO0 SM0、同一个 PIO 程序、同一个 TX/RX FIFO 通路 |
| 每拍 TCK | 4 个 SM 执行周期，与 SWD 位循环一致，因此分频系数不用改 |
| TCK 占空比 | 低 3 拍、高 1 拍（低电平留出 TDO 回传延迟） |
| TDI 与 TDO | 同一次 TCK 内完成「输出 TDI 1 位 + 采样 TDO 1 位」 |
| TMS | 与 `PROBE_SWD_DO_PIN`（GPIO6）复用，只携带电平，不带位流 |
| 指令总量 | PIO 程序 ≤ 32 条且入口地址落在 0–31（`out pc, 5` 只能寻址 5 位）；本方案占 27 条 |

## 2. 硬件通路与模式映射

### 2.1 信号通路

| 信号 | 通路 | 说明 |
| --- | --- | --- |
| SWD_DO / TMS | GPIO6 → 缓冲器 A → 外部 | 缓冲器 A 的通断由 `PROBE_SWD_DOE_PIN`（GPIO4）控制，高 = 正常驱动，低 = 高阻 |
| SWD_DI | 外部 → GPIO5 | 独立输入线，不经缓冲器 |
| TCK/SWCLK | GPIO3，PIO 侧集输出 | 两种模式共用 |
| TDI | GPIO0，JTAG 下归 PIO 输出 TDI；SWD 下归 CDC 桥 UART0 TX | UART TX 在 JTAG 模式改由 GPIO12 输出 |
| TDO | GPIO2，JTAG 下由 PIO 输入 | 独立输入通路，与 DOE 无关；已置 `input_sync_bypass` |
| nTRST | GPIO9，推挽输出 | 本次实现 `PIN_nTRST_OUT()` |
| nRESET | GPIO7，开漏模拟 | 不变 |

TDO 与 SWD_DI 都是独立输入线，不经缓冲器 A，因此读回不受 DOE 影响。而 TMS 与 SWD_DO 共用 GPIO6、必须经缓冲器输出，所以 **JTAG 扫描期间 DOE 必须恒为 1**，从扫描的第一拍保持到最后一拍（TDI 是否也走该缓冲器不影响这条结论）。

### 2.2 模式相关的引脚绑定

| 寄存器 / 配置 | SWD 模式 | JTAG 模式 |
| --- | --- | --- |
| `IN_BASE`（`in pins` 采样起点） | `PROBE_SWD_DI_PIN`(5) | `PROBE_JTAG_TDO_PIN`(2) |
| `OUT_BASE` / `OUT_COUNT`（`out pins`） | `PROBE_SWD_DO_PIN`(6) / 1 | `PROBE_JTAG_TDI_PIN`(0) / 1 |
| `SET_BASE` / `SET_COUNT`（`set pins`） | 6 / 1（不使用） | `PROBE_SWD_DO_PIN`(6) / 1 → TMS |
| GPIO0 功能 | UART0 TX（CDC 桥） | PIO 输出（TDI） |
| GPIO2 方向 | 输入（空闲） | 输入（TDO） |
| GPIO5 方向 | 输入（SWD_DI） | 输入或不驱动 |
| GPIO6 方向 | 输出（SWD_DO） | 输出（TMS） |
| GPIO12 | 高阻输入 | UART0 TX（CDC 桥） |
| GPIO8（GPIO12 缓冲器方向） | 低（输入方向） | 高（输出方向） |

`SET_COUNT` 必须显式配置为 1：默认值为 0，`set pins, 1` 将不驱动任何引脚；若误配成从 GPIO6 起的 5 位，会连带改写 RESET(7)、TRST(9) 和 IIC(10/11)。由于 SWD 模式完全不使用 `set pins`，补上该项不影响已验证的 SWD 行为。

### 2.3 引脚功能切换顺序

**进入 JTAG（`probe_enter_jtag()`）**

1. `probe_wait_idle()`：确认 FIFO 与流水线排空，此时 SM 停在 `get_next_cmd` 的 `pull` 上，不会读到半截命令。
2. 停 SM 后进入引脚改写窗口（`probe_sm_stop()` → `probe_sm_start()`）。
3. `cdc_uart_set_tx_pin(PROBE_UART_TX_JTAG_PIN)`：把 CDC 桥 TX 迁到 GPIO12 并拉高 GPIO8 使能外部缓冲器，随后 GPIO0 脱离 UART。
4. 把 GPIO0 交给 PIO（`pio_gpio_init`）并关掉上拉/下拉。
5. 设置 `PINCTRL`：`in_base = TDO(2)`、`out_base = TDI(0)`、`out_count = 1`。
6. 设置方向：TDI(0) 输出、TMS(6) 输出、TDO(2) 输入，然后启 SM。
7. `jtag_tms_valid = false`：TMS 实际电平未知（可能沿用 SWD 最后一个数据位的电平），下一次 JTAG 操作必须先发一条 TMS 命令。

**进入 SWD（`probe_enter_swd()`）**

1. `probe_wait_idle()`。
2. 停 SM，恢复 `PINCTRL` 与方向：`in_base = SWD_DI(5)`、`out_base = SWD_DO(6)`、`out_count = 1`；TDI(0) 输入、TDO(2) 输入、SWD_DI(5) 输入、DOE(4) 输出、SWD_DO(6) 输出。
3. `cdc_uart_set_tx_pin(PROBE_UART_TX_SWD_PIN)`：GPIO12 置回高阻输入、拉低 GPIO8 撤掉缓冲器输出方向，TX 回到 GPIO0。
4. 启 SM；`jtag_tms_valid = false`；DOE 与 TCK 由 `get_next_cmd` 的 `side 0x0` 回到低电平。

引脚绑定已是 SWD 时 `probe_enter_swd()` 直接返回：SWD 模式下 `probe_gpio_init()` 不接管 GPIO0，TX 保持 `cdc_uart_init()` 建立的 GPIO0。

`cdc_uart_set_tx_pin()` 在引脚变化时按 CDC 线程的复位流程重建串口（中止 DMA、关闭串口、清空环形缓冲、重配引脚、重开串口并重启 DMA 与中断），串口上的在途数据会被丢弃。多核配置下先读当前任务亲和性，未限定在核 0 时切到核 0 再打开中断，退出前恢复原亲和性，避免中断落在不确定的核心上。

## 3. PIO 程序

### 3.1 指令地址表（27 条，地址 0–26）

| 地址 | 指令 | 说明 |
| --- | --- | --- |
| 0 | `nop [1] side 0x0` | `turnaround_cmd` |
| 1 | `jmp x-- 0 [1] side 0x1` | SWD 高阻时钟 |
| 2 | `jmp get_next_cmd` | |
| 3 | `pull` | `write_cmd`（21–128 位，第二个 FIFO 字是数据） |
| 4 | `out pins, 1 [1] side 0x2` | `short_write_cmd`（≤20 位，数据内联在命令字） |
| 5 | `jmp x-- 4 [1] side 0x3` | |
| 6 | `pull side 0x0` | `get_next_cmd`（`.wrap_target`，SWD 空闲时 DOE=0） |
| 7 | `out x, 7` | 取位计数 |
| 8 | `out pc, 5` | 分发到命令入口 |
| 9 | `nop` | `read_cmd` 的循环前导延迟 |
| 10 | `in pins, 1 [1] side 0x1` | SWD 采样 |
| 11 | `jmp x-- 9 side 0x0` | |
| 12 | `push` | |
| 13 | `jmp get_next_cmd` | 读命令出口；没有它会让执行落进下面的 TMS-LOW 命令 |
| 14 | `set pins, 0 side 0x2` | `jtag_tms_lo_cmd`（DOE=1，TCK=0） |
| 15 | `jmp get_next_cmd` | |
| 16 | `set pins, 1 side 0x2` | `jtag_tms_hi_cmd` |
| 17 | `jmp get_next_cmd` | |
| 18 | `set pins, 0 [1] side 0x2` | `jtag_tms_seq_lo_cmd`（TMS=0 一拍，TCK 低） |
| 19 | `out pc, 5 [1] side 0x3` | 取下一拍入口，TCK 高 |
| 20 | `set pins, 1 [1] side 0x2` | `jtag_tms_seq_hi_cmd`（TMS=1 一拍，TCK 低） |
| 21 | `out pc, 5 [1] side 0x3` | 取下一拍入口，TCK 高 |
| 22 | `pull` | `jtag_rw_cmd`（21–32 位，第二个 FIFO 字是 TDI 数据） |
| 23 | `out pins, 1 [1] side 0x2` | `jtag_rw_short_cmd` 入口；TDI ≤20 位内联 |
| 24 | `in pins, 1 side 0x3` | 采样 TDO |
| 25 | `jmp x-- 23` | 循环控制（无 side-set，TCK 保持高，占空比 2/2） |
| 26 | `push` | `.wrap = 26`，回 `.wrap_target` 即 `get_next_cmd` |

### 3.2 源码

```pio
; SPDX-License-Identifier: MIT

.program probe
.side_set 2 opt

public turnaround_cmd:
turnaround_bitloop:
    nop                         [1]  side 0x0
    jmp x-- turnaround_bitloop  [1]  side 0x1
    jmp get_next_cmd

public write_cmd:
    pull
public short_write_cmd:
write_bitloop:
    out pins, 1                 [1]  side 0x2   ; 数据在 TCK 下降沿输出
    jmp x-- write_bitloop       [1]  side 0x3   ; 目标在 TCK 上升沿采样
                                                ; 顺序落到下一条命令
.wrap_target
public get_next_cmd:
    pull                             side 0x0   ; SWCLK 低，DOE 关闭
    out x, 7                                    ; 取位计数
    out pc, 5                                   ; 跳到命令入口

read_bitloop:
    nop                                         ; 补齐循环分支的额外延迟
public read_cmd:
    in pins, 1                  [1]  side 0x1   ; 在 TCK 上升沿采样
    jmp x-- read_bitloop             side 0x0
    push
    jmp get_next_cmd                            ; 读命令出口，避免落进下面的 TMS 命令

public jtag_tms_lo_cmd:
    set pins, 0                      side 0x2   ; DOE=1，TCK=0
    jmp get_next_cmd

public jtag_tms_hi_cmd:
    set pins, 1                      side 0x2
    jmp get_next_cmd

public jtag_tms_seq_lo_cmd:
    set pins, 0                 [1]  side 0x2   ; 2 拍：TMS=0，TCK 低
    out pc, 5                   [1]  side 0x3   ; 2 拍：取下一拍入口，TCK 高

public jtag_tms_seq_hi_cmd:
    set pins, 1                 [1]  side 0x2
    out pc, 5                   [1]  side 0x3

public jtag_rw_cmd:
    pull
public jtag_rw_short_cmd:                       ; DOE 恒为 1，保证 TMS 与 TDI 都能输出
jtag_rw_bitloop:
    out pins, 1                 [1]  side 0x2   ; 2 拍：TCK 低，输出 TDI 的 1 位
    in pins, 1                      side 0x3   ; 1 拍：TCK 高，采样 TDO
    jmp x-- jtag_rw_bitloop         side 0x2   ; 1 拍：TCK 拉低，给 TDO 3 拍回传时间
    push                                        ; 之后由 .wrap 回 get_next_cmd

.wrap                                           ; .wrap = 地址 26
```

`.wrap` 与 `.wrap_target` 的解析规则（`tools/pioasm/pio_assembler.cpp`）：`.wrap` 取「它之前最后一条已定义指令」的地址，`.wrap_target` 取「它之后第一条指令」的地址；两者都是**当前已定义指令**的位置，不会因为后续还有空槽而改变。因此程序长度就是 27 条，地址 27–31 保持未定义。

`write_cmd` 之后没有 `jmp get_next_cmd` 是既有写法：位循环结束时顺序落到下一条命令的首指令（对 SWD 是地址 6 的 `get_next_cmd`），并靠 `jmp x--` 的计数退出保证不越界。读命令与 JTAG 位循环都必须显式回到 `get_next_cmd`（前者用 `jmp`，后者用 `.wrap`）。

### 3.3 `out pc, 5` 的预算与装载偏移

`out pc, 5` 从 OSR 取 5 位写入程序计数器，程序计数器本身只有 5 位（地址 0–31）。因此命令入口地址必须落在 0–31，程序长度上限为 32 条。当前占用 27 条，余量 5 条（例如把 TDO 采样点拆成独立的 5 拍位循环，每增 1 拍需要额外 1 条指令）。

**装载偏移可以非 0**，只要两个条件成立：

1. 加载偏移非负（`pio_add_program()` 空间不足时返回负值，必须断言拦下）；
2. 「偏移 + 最大命令入口」< 32。

原因是两条路径共用同一个 `probe.offset`：

- 程序镜像里的 `jmp` 由 SDK 在装载时重定位（`add_program_at_offset()` 里的 `instr + offset`），例如相对地址 6 的 `get_next_cmd` 在偏移 5 时变成绝对地址 11；
- `out pc, 5` 的目标不在镜像里、由命令字在运行时提供，`fmt_probe_command()` 算的 `cmd_offset[cmd] + probe.offset` 同样是绝对地址 11。

两者相等，`out pc, 5` 才能跳到与 `jmp` 一致的位置。程序长度 27 条决定了偏移最大为 5（`32 - 27`，SDK 自顶向下分配），最大命令入口 23（`jtag_rw_short_cmd`），`5 + 23 = 28`，落在 5 位地址空间内，所以条件 2 由程序长度自动满足。

需要留意的是这条保护**只能来自断言**：入口地址超过 31 时，`out pc, 5` 会静默地按低 5 位取模跳转，表现为难以定位的时序错乱。`probe_init()` 里因此保留了

```c
int offset = pio_add_program(pio0, &probe_program);
hard_assert(offset >= 0);
hard_assert((uint)offset + probe_max_cmd_offset() < 32);
probe.offset = (uint)offset;
```

`probe_max_cmd_offset()` 从同一张命令入口表里取最大值，因此增删命令入口时这个校验会自动跟着变，不需要手工同步。

第 2 条约束：**不得向 FIFO 写入多余字**。地址 13–26 现在有实际语义，任何计划外的 `pio_sm_put_blocking()` 都会造成误跳转。C 侧只需保持「写命令恰好多写一个数据字、读命令不写数据字」的约定。

## 4. 时序分析

### 4.1 JTAG 位循环的 4 拍

`.side_set 2 opt` 下侧集位映射为 bit1 = DOE(GPIO4)、bit0 = TCK(GPIO3)，`side 0x2` = DOE 1 / TCK 0，`side 0x3` = DOE 1 / TCK 1。

| 拍 | 执行指令 | TCK | DOE | 行为 |
| --- | --- | --- | --- | --- |
| T1 | `out pins, 1 [1]` | 0 | 1 | 把 OSR 最低位写到 TDI（`out pins` 在本周期开始写引脚） |
| T2 | （同上，延迟拍） | 0 | 1 | TDI 保持，目标侧建立时间约 2 个 SM 周期 |
| T3 | `in pins, 1 side 0x3` | 1 | 1 | TCK 上升沿；本周期采样 TDO |
| T4 | `jmp x-- jtag_rw_bitloop side 0x2` | 0 | 1 | X 非零则跳回地址 23；TCK 回落 |

要点：

- **TDI 建立时间**：`out pins` 在 T1 写入引脚，目标在 T3 的上升沿采样，中间隔 2 个 SM 周期（分频 1 时约 13 ns），不是最紧的裕量。
- **TDO 采样窗口**：采样点在 T3 开始，TCK 下降沿在 T4 开始，因此 TDO 从下降沿到被采样有 3 个 SM 周期 = `3 × divider × 6.67 ns`：分频 2.0（18.75 MHz）为 40 ns，分频 1.5（25 MHz）为 30 ns，分频 1.0（37.5 MHz）为 20 ns。该窗口必须大于「TCK 输出缓冲 + 线缆 + 目标 TCK→TDO 延迟 + TDO 回传缓冲」之和；板上的 SN74LVC2T45 各按最大 5.8 ns、30 cm 排线按 2 ns 计约 15.6 ns，再叠加目标自身的 TDO 延迟，18.75 MHz 站得住、更高频率需要实测。
- **TDO 建立不足时的现象**：目标在下降沿后尚未更新 TDO，每次采样都读到上一个 bit 的电平，于是整字左移一位、bit0 为移位前的残留电平（IDCODE 由 `0x4BA00477` 变成 `0x974008EF`）。这种偏移是稳定的，靠插填充拍修不好，只能加大低电平窗口或降低 TCK。
- **TCK 高电平 1 拍**：上升沿的采样与 TDI/TMS 建立都在低电平段完成，目标只需要一个上升沿，因此高电平只需 1 个 SM 周期；TMS 本身由 `set pins` 驱动，与侧集无关。
- **DOE 恒定**：4 拍全部 `side` 的 bit1 为 1，TMS 与 TDI 全程可驱动，TDO 采样不受 DOE 影响。
- **TMS 时序**：TMS 只在扫描段之间变化，`set pins` 与随后的 `in/out` 之间天然有 2 个以上 SM 周期，远大于目标对 TMS 的建立时间要求。
- **周期一致性**：SWD 读、SWD 写、JTAG 读、JTAG 写的位循环都是 4 个 SM 周期，因此 `divider = clk_sys / freq / 4` 的分频公式对两种模式通用。

### 4.2 TMS 命令

`set pins, X side 0x2` 只写 GPIO6（`SET_BASE = 6`、`SET_COUNT = 1`）。TMS 有两类命令：

- `jtag_tms_lo_cmd` / `jtag_tms_hi_cmd`：1 个 SM 周期，只改变 TMS 电平，不产生时钟；
- `jtag_tms_seq_lo_cmd` / `jtag_tms_seq_hi_cmd`：`set pins [1]`（2 拍，TCK 低）+ `out pc, 5 [1]`（2 拍，TCK 高）组成一拍“TMS + 时钟”，并用 `out pc` 从命令字的数据段取下一拍入口，一条命令最多串 4 拍。

导航（1,0,0 / 1,1,0,0）与 Update/Idle（1,0）用后者一条命令完成；`SWJ_Sequence` 的等值段在 ≤8 拍时按 4 拍一组走这条路径，更长时用 `jtag_clocks()`（一条 TMS 命令 + 按 128 拍分片的批量时钟）。

### 4.3 指令预算

见 §3.3。要点是命令入口地址必须落在 0–31、程序加载偏移参与命令字编码、不得向 FIFO 写入多余字。当前占用 27 条，余量 5 条。

## 5. 命令字与 C 侧编码

### 5.1 命令字格式（沿用现有布局）

| 位域 | 含义 |
| --- | --- |
| 31:12 | 数据（短命令内联 TDI / SWD 数据；TMS 序列命令则拆成 4 个入口字段） |
| 11:7 | 命令入口地址（`probe_offset_* + probe.offset`） |
| 6:0 | 位计数 − 1 |

位计数字段 7 位，因此单条命令最多 128 位；短命令内联数据占 20 位。

`probe_pio_command_t` 中的命令入口：

```c
typedef enum probe_pio_command {
    CMD_WRITE = 0,          /* 21..128 位，第二个 FIFO 字是数据 */
    CMD_SHORT_WRITE,        /* ≤20 位，数据内联在 [31:12] */
    CMD_SKIP,               /* 保留：直接回到 get_next_cmd */
    CMD_TURNAROUND,         /* SWD 高阻时钟 */
    CMD_READ,               /* SWD 采样 */
    CMD_JTAG_TMS_LO,        /* TMS=0，无时钟 */
    CMD_JTAG_TMS_HI,        /* TMS=1，无时钟 */
    CMD_JTAG_TMS_SEQ_LO,    /* TMS=0 一拍，然后跳到命令字的下一字段 */
    CMD_JTAG_TMS_SEQ_HI,    /* TMS=1 一拍 */
    CMD_JTAG_RW,            /* 21..32 位，第二个 FIFO 字是 TDI */
    CMD_JTAG_RW_SHORT,      /* ≤20 位，TDI 内联在 [31:12] */
} probe_pio_command_t;
```

SWD 与 JTAG 短命令的数据起点一致：7 位计数与 5 位入口地址合计占 12 位，`data << 12` 能让最低位落在位循环读到的第一个位置。

TMS 序列命令把数据段当作入口地址链：n 拍 TMS 依次占用 `[11:7]`、`[16:12]`、`[21:17]`、`[26:22]`，其后一个字段固定填 `get_next_cmd`（n = 4 时是 `[31:27]`），位计数段不参与。因此一条命令最多 4 拍，`jtag_tms_seq()` 只处理 4 拍以内，更长的序列由调用方按 4 拍拆分。

### 5.2 `jtag_cmd()`

```c
/* 一条命令：TCK 走 tck_count 拍（1..32），TMS 保持 tms，TDI 取 tdi_bits 的低 tck_count 位。
 * capture 为真时返回捕获字，bit0 是第 1 个 TCK 采到的 TDO。 */
uint32_t jtag_cmd(uint32_t tck_count, bool tms, uint32_t tdi_bits, bool capture);
```

实现要点：

1. 保护：`tck_count == 0 || tck_count > 32` 直接返回 0，分块由调用方负责。
2. TMS：仅在电平变化时发一条 `CMD_JTAG_TMS_LO/HI` 命令（`bit_count = 1`，命令字不携带数据），并更新 `s_jtag_tms`；同一扫描段内不花任何额外命令。
3. TCK 命令：
   - `tck_count ≤ 20`：`fmt_probe_command(tck_count, CMD_JTAG_RW_SHORT) | ((tdi_bits & mask) << 12)`，`mask = (1u << tck_count) - 1u`。
   - `tck_count > 20`：先写 `CMD_JTAG_RW` 命令字，再写 `tdi_bits` 数据字。
   - 写命令不需要等待完成，依赖 `probe_wait_idle()` 在需要顺序保证时排空。
4. 捕获：`capture` 为真时 `pio_sm_get_blocking()` 读回一个字，`tck_count < 32` 时右移 `32 - tck_count`，使最早采样到的那一位对齐到 bit0（与 `probe_read_bits()` 一致）。
5. 只要走 `CMD_JTAG_RW_SHORT` / `CMD_JTAG_RW`，无论 `capture` 真假都必然执行一次 `push`：不读的写命令会往 RX FIFO 里留下一个无效字。因此 **`capture == false` 时必须使用 `CMD_WRITE` / `CMD_SHORT_WRITE`**（两者同样按 `out pins` + TCK 翻转驱动 TDI，只是低电平为 2 拍、没有 `in pins`，也不 `push`），不能图省事复用 JTAG 命令。否则 RX FIFO 会积累垃圾，后续任何 `jtag_cmd(capture=true)` 都会读到错位的数据字。

   **这条约束的后果比「读到错位数据」严重得多**：`push block` 在 RX FIFO 满（4 个字）时会阻塞状态机，SM 随即不再消费命令，TX FIFO 随之填满，`pio_sm_put_blocking()` 永久阻塞——表现为看门狗复位而不是数据错误。因此 `jtag_bits()` 里必须先按 `capture` 分流，再选命令：

   ```c
   if (!capture) {
       probe_write_bits(n, tdi);   /* CMD_WRITE / CMD_SHORT_WRITE，不 push */
       return 0u;
   }
   /* capture 为真才走 CMD_JTAG_RW_SHORT / CMD_JTAG_RW，并 probe_get() 读走 */
   ```

### 5.3 `JTAG_Sequence()` 的字节缓冲

JTAG 0x14 命令的 TCK 数不超过 64（`JTAG_SEQUENCE_TCK` 是 6 位，0 表示 64），TDI/TDO 缓冲最多 8 字节，因此固定用两个 32 位字处理：

- 输入按 `(bits + 7) / 8` 字节 `memcpy` 到清零的 `tdi_word[0/1]`，第 2 块从 `&tdi[4]` 开始；字内低位对应先发的位（依赖小端）；
- 每块调用 `jtag_bits(bits, tms, word, capture)`：第 1 块为 `min(n, 32)`，第 2 块为 `n - 32`；
- 捕获时把两个结果字 `memcpy` 回 `tdo`，长度 `(n + 7) / 8`；未用到的位由 `jtag_bits` 的右对齐结果保证为 0；
- `tdi == NULL` 时按全 0 输出；`capture` 同时要求 TDO 标志与 `tdo != NULL`。

### 5.4 位级辅助

协议层的尾部处理（如「先补 n 拍 bypass、再补 1 拍 Exit1-DR」）不能再用上述按字节对齐的缓冲路径：尾部的 TDI 值需要从调用方缓冲的中间位偏移开始取，按字节对齐读取会越界。因此 `jtag_bits()` 与 `jtag_tms()` 直接放在 `driver/swd/probe.c` 作为 PIO 原语导出（见 `probe.h`），`jtag_dp_pio.c` 内的薄封装为：

```c
/* n 拍 TDI=1 的填充，n 可为 0，内部按 32 拍分块 */
static void jtag_ones(uint32_t n);
/* n 拍 TDI=0 的填充 */
static void jtag_zeros(uint32_t n);
/* 单拍，TDI 与 TMS 各取一位 */
static void jtag_bit(uint32_t tdi, bool tms);
/* Update-DR/IR 与 Idle 两拍 */
static void jtag_update_idle(void);
/* TMS = 1,1,0,0 的导航，DR 少一拍 */
static void jtag_nav(bool dr);
```

`jtag_ones()` / `jtag_zeros()` 内部按 32 拍分块（`ir_before` / `ir_after` 在 8 器件链上最大可达 255 拍），分块之间 TMS 不变，因此不会产生额外的 TMS 命令。

`jtag_ones()` 用 `jtag_bits(n, tms, 0xFFFFFFFFu, false)`，`jtag_bits` 内部按 `n ≤ 20` 选择短/长命令，与 `jtag_cmd()` 共用编码函数。

## 6. JTAG_DP 五个函数的映射

`lib/CMakeLists.txt` 中把 `JTAG_DP.c` 换成 `module/dap/jtag_dp_pio.c`（保持 `lib/CMSIS-DAP` 原样不动，与 `SW_DP.c` → `sw_dp_pio.c` 的做法一致）。C 文件需要文件头 `@file`/`@brief`，函数实现不强制 Doxygen。

每个入口不再处理时钟：`DAP_SWJ_Clock` 直接写入 PIO 分频（见 §7）。

### 6.1 `JTAG_Sequence()`

TMS 在一次调用内恒定，TDI/TDO 是 LSB first 的字节缓冲。TCK 数 ≤64，固定用两个 32 位字处理：

```c
void JTAG_Sequence(uint32_t info, const uint8_t *tdi, uint8_t *tdo)
{
    uint32_t tdi_word[2] = { 0u, 0u };
    uint32_t tdo_word[2] = { 0u, 0u };
    uint32_t n = info & JTAG_SEQUENCE_TCK;
    const bool tms = (info & JTAG_SEQUENCE_TMS) != 0U;
    const bool cap = ((info & JTAG_SEQUENCE_TDO) != 0U) && (tdo != NULL);
    uint32_t lo_bits;

    if (n == 0U) {
        n = 64U;
    }
    lo_bits = (n > 32U) ? 32U : n;

    if (tdi != NULL) {
        memcpy(&tdi_word[0], tdi, (lo_bits + 7U) >> 3);
        if (n > 32U) {
            memcpy(&tdi_word[1], &tdi[4], ((n - 32U) + 7U) >> 3);
        }
    }

    tdo_word[0] = jtag_bits(lo_bits, tms, tdi_word[0], cap);
    if (n > 32U) {
        tdo_word[1] = jtag_bits(n - 32U, tms, tdi_word[1], cap);
    }

    if (cap) {
        memcpy(tdo, tdo_word, (n + 7U) >> 3);
    }
}
```

`tdo` 非空但 `tdi` 为空的情况按 CMSIS-DAP 语义不存在，`tdi == NULL` 按全 0 输出的分支仍能兜住。

### 6.2 `SWJ_Sequence()`

这是唯一需要逐位 TMS 的入口：数据字节流的每一位都是 TMS 电平（LSB first）。实现方式是按**等值段**切分，段内 TDI 不关心（TAP 在 Reset/Idle/Run-Test 状态下忽略 TDI）：

- 段长 ≤8 时按 4 拍一组调用 `jtag_tms_seq()`；
- 更长时调用 `jtag_clocks(n_seg, tms)`：一条 TMS 命令 + 按 128 拍分片的 `probe_write_bits()`。
- 状态跨调用保存在 `s_jtag_tms`，所以相邻调用之间不会多发 TMS 命令。
- `DAP.c` 的 `DAP_SWJ_Sequence()` 在 SWD 与 JTAG 两种模式下都会调用 `SWJ_Sequence()`，因此该函数的实现需要按 `DAP_Data.debug_port` 分流：SWD 下沿用现有逐字节 `probe_write_bits()` 路径，JTAG 下走上面的等值段路径。
- `DAP_PORT_DISABLED` 下不产生任何时序。

### 6.3 `JTAG_IR()`

上游语义（`JTAG_DP.c` 的 `JTAG_IR_Function`）：导航 4 拍（TMS=1,1,0,0）→ 前导 bypass → IR 位（末位 TMS=1）→ 后置 bypass → Update-IR（TMS=1）→ Idle（TMS=0）。映射为：

```c
void JTAG_IR(uint32_t ir)
{
    const uint32_t before = DAP_Data.jtag_dev.ir_before[DAP_Data.jtag_dev.index];
    const uint32_t length = DAP_Data.jtag_dev.ir_length[DAP_Data.jtag_dev.index];
    const uint32_t after  = DAP_Data.jtag_dev.ir_after [DAP_Data.jtag_dev.index];

    jtag_nav(false);                                    /* TMS=1,1 两拍 + TMS=0,0 两拍，TDI 无关 */
    jtag_ones(before);                                  /* bypass 前：TDI=1，TMS=0 */

    if (length != 0U) {
        jtag_bits(length - 1U, false, ir, false);       /* IR 低位（LSB first） */
    }
    jtag_bit((length ? (ir >> (length - 1U)) : 0U) & 1U, true);   /* 末位 + Exit1-IR */

    if (after != 0U) {
        jtag_zeros(after - 1U);                         /* bypass 后，不含末拍 */
        jtag_bit(1U, true);                             /* 末拍：Exit1-IR */
    }
    jtag_update_idle();                                 /* Update-IR + Idle */
}
```

与上游的差异：

- **TDI 默认值**：上游在每次 `JTAG_IR()` 结束时把 TDI 置 1，并在 bypass 段发送 TDI=1。本方案的 `jtag_ones()` 与上游一致发 1；IR 之后紧接着的 `JTAG_Transfer()` 会自己从 `jtag_nav(true)` 重新开始，不受影响。
- **导航拍的 TDI**：上游导航拍不做 `PIN_TDI_OUT()`，TDI 保持上一次的值（`probe_write_bits()` 只送时钟，不改变 TDI 引脚电平）；与上游一致。
- **`after` 处理**：上游在 `after == 0` 时把最后一位 IR 与 Exit1-IR 合成一拍，在 `after != 0` 时先发完全部 IR 位、再发 `after - 1` 拍 bypass，最后补一拍 Exit1-IR。上面的实现两种情形都先发 `length - 1` 位、再把第 `length` 位与 Exit1-IR 合成一拍，之后把 `after` 拍全部当作 bypass（`after` 拍里的最后一拍仍是 TMS=0）。两种写法在 TAP 上等价：`after` 段的位内容对 IR 无影响，末拍 TMS=1 的位置只取决于 `after` 是否为 0，上一种写法里该末拍被提前合并了。

### 6.4 `JTAG_ReadIDCode()` / `JTAG_WriteAbort()`

按上游语义重写：

- `JTAG_ReadIDCode()`：导航（TMS=1 一拍 + TMS=0 两拍）→ `index` 拍 bypass → 32 拍读（TMS=0 的 31 拍 + 末拍与 TMS=1 同拍）→ bypass → Update-DR → Idle。
- `JTAG_WriteAbort()`：导航 → `index` 拍 bypass → TDI=0 的三拍（RnW=0、A2=0、A3=0）→ 32 位数据（末拍与 TMS=1 同拍）→ bypass → Update-DR → Idle。
- `JTAG_ReadIDCode()` 用 `jtag_bits(31u, false, 0u, true)` 取 D0..D30；`JTAG_WriteAbort()` 用 `jtag_bits(31u, false, data, false)` 批量输出 D0..D30，D31 与 Exit1-DR 同拍（IDCODE 的 D31 由上游同样丢弃）。

### 6.5 `JTAG_Transfer()`

上游顺序：TMS=1 一拍（Select-DR）→ TMS=0 两拍（Capture-DR、Shift-DR）→ `index` 拍 bypass → `request >> 1`、`request >> 2`、`request >> 3` 三拍分别送出 RnW/A2/A3 并采回 ACK.0/1/2 → 数据 32 拍（末拍 TMS=1）→ `count - index - 1` 拍 bypass → Update-DR → Idle → `idle_cycles`。

映射要点：

- 三拍 ACK 用一次 `jtag_bits(3, false, request >> 1, true)` 发出并捕获，`& 0x7` 即 ACK（bit0 = ACK.0），与上游 `ack = (bit<<1) | (bit<<0) | (bit<<2)` 的组装结果一致。
- ACK 非 OK 时不发数据段，直接进 Exit1-DR（TMS=1）→ Update-DR → Idle。
- 读事务（`jtag_read_data()`）：`jtag_bits(31, false, 0, true)` 取 D0..D30，返回值的 bit0 就是 D0；随后 1 拍 TMS=1 取 D31（与 Exit1-DR 同拍，该位与 IDCODE 同样丢弃），再走 `n_after` 拍 bypass（TMS=0）。
- 写事务（`jtag_write_data()`）：D0..D30 由一条 `CMD_WRITE` 批量发出，D31 与 Exit1-DR 同拍（TMS=1），再走 `n_after` 拍 bypass。
- `idle_cycles`：`jtag_tms(0)` + `probe_write_bits(n, 0)`，超过 128 拍分片；TDI=0、TMS=0。
- 时间戳：`DAP_TRANSFER_TIMESTAMP` 时按上游填 `DAP_Data.timestamp = TIMESTAMP_GET()`。

### 6.6 配置项改动

| 文件 | 改动 |
| --- | --- |
| `config/DAP_config.h` | `DAP_JTAG` 由 0 改为 1；`PORT_JTAG_SETUP()` 为 `probe_init(); probe_enter_jtag();`；`PORT_SWD_SETUP()` 为 `probe_init(); probe_enter_swd();`；`PIN_nTRST_OUT(bit)` 驱动 GPIO9；各 `*_IN()` 从空实现改为 `gpio_get()`；`DAP_DEFAULT_PORT` 保持 1（SWD） |
| `config/dap_board.h` | `PROBE_UART_TX_SWD_PIN`=0、`PROBE_UART_TX_JTAG_PIN`=12、`PROBE_UART_TX_OE_PIN`=8；其余引脚不变 |
| `lib/CMakeLists.txt` | `JTAG_DP.c` 换出，改由 `module/dap/jtag_dp_pio.c` 提供 |
| `module/CMakeLists.txt` | 加入 `dap/jtag_dp_pio.c` |
| `driver/swd/probe.c` / `probe.h` | PIO 分频按 Hz 设置、移除 `cached_delay` / `probe_apply_clock()`、新增 JTAG 原语 |
| `driver/swd/probe_oen.pio` | 补 `SET_COUNT`、3 条 JTAG 命令 |
| `driver/uart/cdc_uart_v2.c` | 新增 `cdc_uart_set_tx_pin()`：TX 在 GPIO0 / GPIO12 之间切换，引脚变化时按 CDC 线程流程重建串口，并驱动 GPIO8 缓冲器方向 |
| `module/dap/sw_dp_pio.c` | 移除时钟检查；`SWJ_Sequence()` 在 JTAG 模式下转调 `JTAG_SWJ_Sequence()` |

`DAP_SWJ_Pins()` 里的 `PIN_SWCLK_TCK_SET/CLR`、`PIN_SWDIO_TMS_SET/CLR`、`PIN_TDI_OUT()` 仍是空实现：这些引脚在两种模式下都由 PIO 独占，主机侧不应通过 `DAP_SWJ_Pins` 直接翻转。`PIN_nTRST_OUT()` 例外，它是独立 GPIO。各 `*_IN()` 改为读 GPIO 后，`DAP_SWJ_Pins` 的 `wait` 与回读值才有意义（此前恒为 0，带 wait 的请求会一直空转到超时）。

## 7. 时钟与分频

两种模式共用同一分频寄存器。`DAP_SWJ_Clock` 经构建时补丁替换的 `Set_Clock_Delay()` 直接调用 `probe_set_swclk_freq_hz(clock)`，不再经过 `clock_delay` 量化：

```c
void probe_set_swclk_freq_hz(uint32_t freq_hz);
```

- 分频值按 `divider = clk_sys / (4 × swclk)` 以 1/256 为单位向上取整，保证实际频率不大于请求值。
- `probe_set_tck_freq()` / `probe_get_tck_freq()` 是 `probe_set_swclk_freq()` / `probe_get_swclk_freq()` 的语义别名。
- 最高 TCK 频率受 4 周期下限约束，理论值 `clk_sys / 4 = 37.5 MHz`（分频 1.0）。
- 最低频率为 `PROBE_SWCLK_MIN_KHZ = 10 kHz`（分频 3750）：再低则一次握手可能要排空整条队列，耗时超出手握超时与看门狗窗口；低于该值的请求按 10 kHz 下发，回读频率随之为 10 kHz。
- 握手超时按分频计算：`2 × 2048` 个 SM 周期换算成微秒，钳在 1 ms～500 ms，10 kHz 时约 102 ms；分频改写时同步刷新，见 `docs/design.md` §7.2。
- `probe_init()` 重新初始化状态机后再次下发最近一次请求的频率；`PORT_SWD_SETUP()` / `PORT_JTAG_SETUP()` 不需要清任何缓存。
- 分频寄存器在 SM 运行期间改写是安全的；`DAP_SWJ_Clock` 处理时立即生效，各操作入口没有额外等待。

## 8. 验证方案

JTAG 时序无法用 cmock 在主机上覆盖，以实机测试为准。

### 8.1 实机测试（编写测试用例文档）
| 用例 | 方法 | 预期 |
| --- | --- | --- |
| 波形核对 | 逻辑分析仪接 TCK/TMS/TDI/TDO | JTAG 扫描期间 DOE 恒 1、TCK 低 3 拍高 1 拍；TDI 在 TCK 上升沿前已稳定；TDO 在 TCK 下降沿后更新，并在下一个上升沿前被采样 |
| IDCODE | `DAP_JTAG_Configure` 后 `DAP_JTAG_IDCODE` | 与目标器件手册一致；单器件链（`ir_before`/`ir_after` 为 0）与多器件链各测一次 |
| 位对齐 | 连续读两次同一寄存器的值 | 两次结果一致；若整体偏移一位，按 §4.1 的补偿方式加 1 拍填充 |
| 模式切换 | 反复 `DAP_Connect(SWD)` / `DAP_Connect(JTAG)` | SWD 功能不受影响；GPIO0 在 SWD 下为 UART0 TX、JTAG 下为 PIO 输出的 TDI；CDC 桥 TX 随之在 GPIO0 / GPIO12 之间切换 |
| 串口桥 | 两种模式下分别收发 CDC 数据 | SWD 模式下 GPIO0 有 UART 数据；JTAG 模式下 GPIO12 有 UART 数据且 GPIO8 为高；切换后不丢字节、不出现半截字符 |
| IDCODE 自检 | TLR 之后**不装载 IR**，直接走 Shift-DR 读 32 位 | 读出目标真实 IDCODE；硬编码 IR 长度会把指令装错、DR 读回全 0 |
| SWJ 序列 | 在 JTAG 模式下下发 `DAP_SWJ_Sequence` 切换序列 | TMS 电平序列与请求数据逐位一致；≤8 拍的等值段由 TMS 序列命令完成，更长的等值段用「设 TMS + 按 128 拍分片的批量时钟」 |
| nTRST | `DAP_SWJ_Pins` 操作 nTRST | GPIO9 电平随命令变化，且不影响 PIO 时序 |
| 时钟 | 设置多个 `DAP_SWJ_Clock` | TCK 频率随设定值变化，两模式在同一设定下频率一致 |
| 低频下限 | 下发 `DAP_SWJ_Clock(1000)`（1 kHz）、`DAP_SWJ_Clock(500)` | 实际 TCK 均为 10 kHz（`PROBE_SWCLK_MIN_KHZ`），调试会话仍可正常读写；`probe_get_tck_freq()` 返回 10 |
| 握手超时 | 低频（10 kHz）+ 多次带 idle_cycles 的写，再切协议 | 不出现卡死、不复位；最坏一次握手等待约 102 ms，看门狗不触发 |

## 9. 已确认的前提

| 项目 | 结论 |
| --- | --- |
| TDI 引脚 | GPIO0 在 JTAG 下归 PIO 作 TDI；SWD 下归 CDC 桥作 UART0 TX，UART TX 在 JTAG 模式迁到 GPIO12 |
| TDO 通路 | 独立输入，不经过缓冲器 A，与 DOE 无关 |
| nTRST | 实现 `PIN_nTRST_OUT()` 为 GPIO9 推挽输出 |
| `SET_COUNT` | 显式配置为 1，基址 `PROBE_SWD_DO_PIN` |
| 时钟语义 | 沿用现有系数，两模式一致，不修正绝对值偏差 |
| PIO 地址预算 | 程序 27 条（地址 0–26），入口地址全部落在 0–31，余量 5 条 |
| 程序加载地址 | 可以非 0，`probe_init()` 用断言保证「偏移 + 最大命令入口 < 32」 |

## 10. 实施顺序建议

1. 改 `probe_oen.pio`：补 `SET_COUNT`、新增 JTAG 命令入口（含 TMS 序列），用 `pioasm` 核对地址表与程序长度。
2. 改 `probe.c` / `probe.h`：按 Hz 设置 PIO 分频、`probe_set_tck_freq()`、`probe_enter_jtag()` / `probe_enter_swd()`、`jtag_bits()`、`jtag_tms_seq()`。
3. 改 `config/dap_board.h`、`config/DAP_config.h`：`DAP_JTAG = 1`、`PORT_JTAG_SETUP()`、`PIN_nTRST_OUT()`、UART TX 双引脚定义与 GPIO8 缓冲器方向脚。
4. 新增 `module/dap/jtag_dp_pio.c`：五个 JTAG 函数 + `SWJ_Sequence()` 的模式分流。
5. 改 `lib/CMakeLists.txt`：`JTAG_DP.c` 换出。
6. 先跑主机侧编码/解码测试，再上实机核对波形、IDCODE 与模式切换。
