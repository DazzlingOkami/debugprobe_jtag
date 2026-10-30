# DAP 线程喂狗与常开任务测试

这两项行为依赖实机上的 USB 与 SWD 时序，无法用 cmock 在主机上覆盖，按下面的步骤在实机上验证。

## 1. 密集 SWD 请求下看门狗不触发

DAP 线程绑定核 1 且优先级高于 `WDOG`，请求持续不断时 `WDOG` 无法在核 1 运行，只能由 DAP 线程自己每 500 ms 喂狗。

准备：目标板保持 SWD 连接，UART1（GPIO20，115200）接串口终端。

1. 烧录固件并复位，确认 UART 打印启动日志。
2. 用 Bulk 压测工具持续发送真实的 `DAP_TransferBlock` 写包（`--addr` 换成目标可写的 SRAM 地址）：

   ```text
   tools\usb_bulk_bench\usb_bulk_bench.exe -m swd -d 4 -t 600 --addr 0x24000000 --words 14 --swclk 20000000
   ```

3. 观察 10 分钟：工具应正常结束并报告 0 errors，UART 不应出现 `watch dog timeout. reset boot.`，USB 设备不应重新枚举或出现 RPI-RP2 盘符。

判读：去掉 DAP 线程里的喂狗后，`WDOG` 会被挤在核 1 之外，同样的压测应在 1 s 左右的看门狗超时后复位。若保留喂狗仍出现超时，说明单条命令的阻塞时间超过 1 s 的看门狗窗口，需要检查 `probe.c` 的握手超时。

## 2. 反复断开与重连

DAP 线程只在 `usb_init()` 中创建一次，挂载、卸载、挂起回调不创建、挂起或删除它。

1. 连接 OpenOCD 或 pyOCD，确认能正常 halt/resume。
2. 拔掉 USB 再插回，重复 10 次；每次插回后执行：

   ```powershell
   openocd -c "adapter driver cmsis-dap; transport select swd" -c "init" `
           -c "cmsis-dap cmd 0x00 0x01" -c "shutdown"
   ```

3. 预期：每次都能读到 `DAP_Info` 的 Vendor 字符串 `Cassini`，过程中无 `stack overflow`、`Malloc Failed` 或看门狗复位日志。

## 3. 传输中断开

1. 在 `-m swd` 压测运行期间直接拔掉 USB。
2. 重新插回并再次连接 OpenOCD。

预期：设备重新枚举后 DAP 命令恢复正常；压测工具报 USB 错误退出属正常现象。无请求时 DAP 线程只阻塞在通知等待，不占用 CPU。
