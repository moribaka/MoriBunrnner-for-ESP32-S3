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

### 第一阶段完成：ESP 接收流水线

代码提交 `32a3116`。只通过串口 `cpld-read-bench-gba` 启用；
`bacon_cpld_try_transfer_locked()` 原函数未改动，仅在文件末尾包含独立试验实现。
没有修改共享 SPI 后端。候选直接使用已独占的 SPI 设备，保留原 CS 建立/保持
各 5 us、READY 检查、四字节 dummy、逐包 CRC32 和 DONE 完成量核对。
缓冲为两块独立内部 DMA 内存；上一包 CRC/复制与下一包接收重叠。
即使上一包 CRC 错误，也先收尾已经启动的只读 DMA，再退出模式并释放资源。

设备 CPU 性能锁上限实际为 160 MHz，APB 80 MHz；SPI 配置/实际均为 40 MHz。
基准运行在串口任务 CPU0，输出目的地为 PSRAM，每流 64 KiB。
正常烧录任务的 CPU 核、文件 I/O、调度和任务耗时不包含在以下数据中。

每轮先用原 Bacon 对照起点和 32 MiB 末端的长度
`2,4,6,1018,1020,1022,2040,2042,65536`，两个候选共 36 项。
随后原 BSC1 和两个候选分别读取前 32 MiB，每 64 KiB 做逐字节比较，
两个候选的先后顺序交替。两轮均 `ok=true`、`checked_bytes=33554432`、
`boundary_passes=36`。全量参考是本轮原 BSC1 读取，不是旧备份文件；
原 Bacon 的独立对照覆盖边界样本，不是额外的 32 MiB 全量原 Bacon 对照。

| 方案 | 第一轮秒 / MB/s | 第二轮秒 / MB/s |
| --- | ---: | ---: |
| 原 BSC1 | 13.051565 / 2.571 | 12.916985 / 2.598 |
| 流内独占总线、串行处理 | 13.393061 / 2.505 | 13.434931 / 2.498 |
| 流内独占总线、接收流水线 | 10.053817 / 3.337 | 10.123334 / 3.315 |

MB/s 按十进制字节计算；流水线对应 3.16–3.18 MiB/s。
相对原路径吞吐提升约 27.6–29.8%，32 MiB 读取少约 2.79–3.00 秒。
**尚未达到 4 MB/s，也没有把候选接入正常 dump、verify 或烧录。**
独占总线的候选还包含计时、显式内部缓冲和逐包公平调度，因此这是完整候选
路径对照，不能把其约 3–4% 退步精确归因于 acquire_bus 本身。

第一轮流水线分项：READY 等待/状态查询 1.263 s，事务启动（含 CS 建立）
0.993 s，DMA 收尾等待（含 CS 保持）3.911 s，CRC 2.166 s，复制 0.943 s。
CRC/复制与 DMA 在时间上重叠；这些数不是完整分区，不能简单相加解释总耗时。
状态查询约 3.6 万次；物理载荷线速下限约 6.71 s。

### 主机与构建验证

- ESP-IDF 5.5.1 构建成功；试验版本 `v2.32.131-readexp1`。
- 主机测试直接包含实际 `bacon_cpld_read_experiment.inc`，覆盖上述边界长度、
  非对齐输出指针、输出哨兵、首/尾包 CRC、DMA 启动失败、READY 超时、
  部分模式进入失败、退出失败、取消、内存分配失败及首错误保留。
  验证没有带着在途 DMA 释放 CS/总线/缓冲或调用 yield。
- 原 BSC1 描述符字节序/CRC 测试通过；`git diff --check` 通过。
- 两轮后 `cpld-info` 成功，PSRAM 空闲恢复为 7539336 字节，最大内部空闲块
  保持 31744 字节。正常卡带写入、GB/GBC、音乐和 UI 全套回归未执行；
  这些生产实现保持原样，试验结果不作为其新版资格声明。

复现主机测试（在仓库根目录）：

```powershell
gcc -std=c11 -O2 -Wall -Wextra -Werror -I tests/ag32_program_host/stubs -I main -I shared tests/ag32_protocol_host/test_cpld_read_experiment.c -o build/test_cpld_read_experiment.exe
./build/test_cpld_read_experiment.exe
```

加载本机 IDF 环境后构建：`idf.py '-DPROJECT_VER=v2.32.131-readexp1' build`。
确认插入 GBA 卡且无任务后，只刷应用，再运行：

```powershell
& 'F:\ESP32\tools\v5.5.1\python_env\idf5.5_py3.14_env\Scripts\python.exe' tools/serial_debug.py --port COM26 --timeout 150 --log docs/cpld_read_experiment_20260912.serial.log cpld-read-bench-gba
```

### 工件与证据

| 工件 | SHA256 |
| --- | --- |
| 原 v2.32.131 应用 | `110EC67A5B6A98692B2B2B7A39550D5C9FC0541216B5404FEBC3221ADB46573E` |
| readexp1 应用，2843776 字节 | `FF014CD33B92DB8449D629A58E0E2BC02CECD6DECE31CA26D7D161CB26A5256A` |
| 未更新的 AG32 batch | `94EFE7052DF14813D0829E175BA520F8EE8B5C2B2C6D5A68E17C2604A43B3907` |

应用备份分别在 `.tmp-cpld-read/baseline-v2.32.131.bin` 和
`.tmp-cpld-read/readexp1.bin`，不作为发布包。
原始串口证据：`cpld_read_experiment_20260912.serial.log`；
两轮结构化结果：`cpld_read_experiment_20260912.results.json`；
构建/刷写/主机测试日志：`cpld_read_experiment_20260912/`。

## 下一阶段：4 MB/s 的协议试验依据（尚未实现）

32 MiB 在 4 MB/s 下需 8.389 s；相对当前约 10.05–10.12 s，
还要减少约 1.67–1.73 s。仅去掉 8 字节 dummy/CRC 不足以完成目标。

建议新增只读 BSC2 能力/命令，旧 BSC1 和所有写命令完全保留：

1. 一次 DMA 接收多帧，减少约 3.3 万次独立数据事务和逐包状态轮询。
2. 帧含序号、有效长度、状态和 CRC；明确完整帧后才释放双缓冲的消费槽。
3. 采用已就绪额度或固定尺寸空闲帧处理供数不足，禁止依赖“卡带应该够快”
   而让 ESP 在欠载时误收旧数据。SPI 从机不能暂停主机的时钟。
4. 先仿真 CS 中断、时钟暂停、欠载、CRC、尾包和两缓冲周转，再走完整
   AG32 布线/打包流程，记录实际资源和时序。现有 129/132 LogicTILE 与
   系统负 setup 余量是旧报告事实，不能假定新逻辑自动可布通。
5. 新能力未通过探测或未显式选择时沿用原路径；错误终止，不在操作中换协议。

本阶段不改 RTL，因此没有新增布线结果，也没有声称解决旧系统时序违例。

## 回退

已在两轮完成、设备空闲后，仅将上述基线应用重新刷到 `0x20000`。
esptool 报告 `Hash of data verified`；串口回读版本 `v2.32.131`，
`burn_running=false`、`patch_running=false`，`cpld-info` 再次通过，
SPI 实际 40 MHz。没有刷 bootloader、分区或 AG32。

最终设备运行原固件；Git 分支保留诊断源码。`build/` 仍是 readexp1 构建，
不要将其误当作设备当前固件或正式发布包。试验二进制和基线备份都在
`.tmp-cpld-read/`；后续正式构建必须明确版本与启用范围。
