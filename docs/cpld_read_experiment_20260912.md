# CPLD 读取吞吐试验（2026-09-12）

## 范围与基线

用户要求提高 ESP32/CPLD 读取吞吐，避免影响其他功能，保存本地 Git 和 MD。
基线提交 `09cb43d`；试验分支 `codex/cpld-read-experiment-20260912`。
已有未跟踪文件 `.workbuddy/`、Python 缓存、`tools/capture_reset.py` 不纳入提交。

ESP 基线 v2.32.131，应用 SHA256
`110EC67A5B6A98692B2B2B7A39550D5C9FC0541216B5404FEBC3221ADB46573E`；
备份在 `.tmp-cpld-read/baseline-v2.32.131.bin`。
COM26 状态确认无烧录任务；UI 当前 GBA 卡 ID `89 00 7E 22 28 22 01 22`，
型号 MT28EW01GABA，128 MiB，1024 B 编程缓冲。

第一轮仅增加显式串口诊断读取路径，正常读写入口保留原实现。
不改 CPLD 位流、40 MHz SPI、卡带时序、烧录/擦除、mapper、UI 或持久配置。
比较原始读取、流内独占 SPI 总线、DMA 接收与上一包 CRC/复制重叠三个方案。
CRC 错误终止；已经发起的 DMA 必须结束后才能释放 CS、缓冲或 SPI 总线。

## 已发现的基线限制

原有 `cpld-check` 在读取测试前的 MCU halt 步骤超时，passes=0，
`mcu_halted=false`、`mcu_resumed=false`，不能作为通过记录。
SWD DP 仍返回 `0x2ba01477`，DMI 状态等待失败；本轮不改此独立问题。
随后 `cpld-info` 成功，BSC1 身份可正常读取。
新吞吐诊断不操作 SWD/MCU；其结果不等价于“暂停 MCU 读取验证”。
原始输出保存在 `cpld_read_experiment_20260912.serial.log`。

## 验证与结果

待追加构建、逐块对照、边界长度、错误清理测试和实测数据。

## 回退

正常功能默认不启用试验路径。需要移除诊断固件时，确认设备无任务后，
仅将上述基线应用重新刷到 `0x20000`；不刷 bootloader、分区或 AG32。
