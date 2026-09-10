# BSC1 纯 CPLD 协议实现与实机验证

纯 CPLD 连续协议已在 GBA 卡上完成 32 MiB 原始写入和全量校验。
最终 ESP 接收优化版本已通过不同 ROM、补丁、PSRAM/Pipeline/Direct 流程测试。
原卡已恢复并全量校验；最终同条件对照见 [测试表](cpld_protocol_results_20260910.md)。
SPI 保持 40 MHz，AG32 系统保持 150 MHz。

## 实现边界

原 `bacon_legacy_core` 主体与 1b4f7f9 逐字一致（忽略换行格式）。
模式识别和总线所有权切换位于外层；短访问继续使用原 Bacon，
大块读取和标准 AMD 缓冲编程由新 CPLD 引擎执行。MCU 不接收数据、
生成卡带脉冲或轮询每页完成；暂停 MCU 的读取测试已经通过。

BSC1 每个流只发一次地址、长度和操作。上位机常规烧录按 64 KiB 建流，
后续包只有数据、对齐填充和 CRC32。两个 1024 字节 BRAM 缓冲让接收下一页
与当前页编程重叠。读取每包 1020 字节，余下 4 字节 RAM 保存 CRC。
描述符与数据 CRC 通过后才允许相应写入；错误终止，不自动重试或中途换协议。

线协议的唯一详细定义在 [BACON_CPLD_STREAM_DESIGN.md](../shared/BACON_CPLD_STREAM_DESIGN.md)。
`legacy` 指定原 Bacon，`cpld` 指定扩展，`auto` 按能力和算法选择。
默认配置已改为 auto；设备已保存 auto / PSRAM 配置，回读确认实际大块后端为 cpld。
保存配置后的 32 MiB 自动模式校验通过，耗时 23342 ms。
显式 CPLD 烧录在擦除前拒绝不支持的页大小、Intel/GBX、交换数据线等配方。
实验 MCU 数据平面仍保留源码，但本位流以 `ENABLE_MCU_TRANSPORT=0` 排除，
避免超过 AG32 的物理容量。

ESP 使用独立的零值 TX 时钟缓冲、对齐 RX 缓冲和 ROM CRC32 函数。
SPI 后端只在 RX 位于可 DMA 内存、字对齐且不与 TX 重叠时直接接收；
其余情况沿用既有接收缓冲，保持长度和别名约束。

## 当前硬件与工件

- ESP32-S3：COM26，MAC a4:cb:8f:f2:c4:c0；本轮 HTTP 为 192.168.1.134。
- 卡带 ID：89 00 7E 22 28 22 01 22；CFI AMD 128 MiB，128 KiB 扇区，
  1024 字节缓冲，word-address / low-lane / 无 D0-D1 交换。
- 此卡与早先 512 字节缓冲的测试卡不同，不能直接合并速度结论。
- 原卡完整 32 MiB 备份：`/sdcard/ROM_OUTPUT/cpld_before_20260910.gba`；
  内容与 TF 上的 Pokemon 中文典藏版源文件一致。
- AG32 TF 工件：`/sdcard/ag32_cpld_posedge_20260910.bin`。
- 当前 batch：`example/moriburnner_ag32_batch.bin`，109692 字节。
- 当前 ESP app：`build/moriburnner.bin`，2794672 字节，仅刷入 0x20000。
  TF 的 .web/main.html 已更新并回读核对一致。

| 工件 | SHA256 |
| --- | --- |
| AG32 batch | 94EFE7052DF14813D0829E175BA520F8EE8B5C2B2C6D5A68E17C2604A43B3907 |
| CPLD | 0058F32B2604F7F76431B326075BC852ECFD8AFF85574AB5A43EA997061436D7 |
| MCU | F0B69F07350F85AA05D663D9C37208B964B8D84FB2BD6BAFC7FD8A56C2A79DA4 |
| 最终 ESP app | B929E5E380181DAB21C9FC90EC30E8B87F0FF4A832C10705799AA1C7B5C31E19 |

完整流程依次执行 prelogic、MCU release、af_quartus.tcl（含 af_ip.tcl）、
Supra FLOW ALL / MODE QUARTUS / seed42 / timing_more / hybrid / highest，
再 buildbatch。主机解析器确认 3 个记录和 MCU/CPLD 原始内容完全匹配，
打包前后两份输入 hash 不变。ESP 端 TF 检查通过后经板载 SWD 更新：
DP 2ba01477、device 40200001、3 records，
programmed=verified=total=108924，recovery_required=false。
ESP 两次 app 更新均通过 esptool hash 校验。

## 实际布线结果

1930/2112 逻辑单元，129/132 LogicTILE，1917 LUT，934 寄存器，
2/4 BRAM，1/1 PLL，44/128 引脚。

系统 setup -1.122 ns、hold +0.334 ns；SPI setup +14.099 ns、hold +0.615 ns。
最差系统路径是读取结束阶段至剩余字节计数更新。自动生成的 CS 时钟路径
另见报告。该位流按硬件测试版使用，实机通过不等同于全部 STA 时序合格。
没有降低时钟，也没有添加 multicycle/false-path 例外来隐藏新引擎路径。
源码、编译日志、完整时序及摘要保存在 `docs/cpld_posedge_20260910/`。

## 实机发现及修复

首个下降沿发送候选的身份探测通过，暂停 MCU 后一度连续读过 512 KiB，
但另一次在 128 KiB 后把 BA CE 读成 FA CE，状态 flags 也有位错误；
完整 32 MiB 校验随即在首块失败。因此没有用该候选写卡。

原 Bacon 在主机上升沿采样当前位后更新下一位 MISO，新扩展最初在下降沿
更新，只有半个周期的返回建立时间。扩展改为原 Bacon 的上升沿后更新方式，
保留主机 mode0、40 MHz 和所有包格式。仿真主机同步改为在采样边沿读取，
而非延迟 2 ns 读取已更新的下一位。假设 14 ns 回程预算时，旧候选在第一份
状态返回就失败，新候选通过全部读写测试；14 ns 是测试预算，不是板上测量值。

修复后，32 MiB 全量读取、32 MiB 擦除/编程及其全量校验均通过，
未再次观察到前述状态错误。最终 ESP 接收优化后，暂停 MCU 的
8×64 KiB 读取通过，耗时 204719 us（优化前 258717 us），首缓冲就绪 393 us。
256 次原 Bacon 单字读取中位数 36 us，P95 38 us。
首缓冲就绪与单字访问延迟是不同指标。

## 已完成的首轮同卡对照

以下两行是 ESP 接收优化前、同一备份 ROM、强制擦除、PSRAM 流程，
均完成 33554432 字节写入和全量校验。编程时间取串口 program_ms，
不能用 HTTP write_time_ms（TF 阶段时间）代替。

| 协议 | 擦除 ms | 编程 ms | 任务 ms | 全量校验 ms |
| --- | ---: | ---: | ---: | ---: |
| 原 Bacon | 37328 | 43633 | 81852 | 33827 |
| CPLD 连续 | 37392 | 22113 | 60385 | 27753 |

编程速度约 1.97 倍；整项烧录任务时间减少约 26%。
旧逐页 Bacon profile 的 calls/avg_once 不计新引擎，不应把其中的零解释成未编程。
最终 ESP 的公平对照与补丁矩阵已使用独立 final-* label 追加到原始记录。
同卡同 ROM 最终原 Bacon 编程 43394 ms、新 CPLD 21294 ms（2.04 倍）；
任务 82582→59686 ms，校验 33658→23420 ms。其他 ROM 和流程全部全量通过。

## 仿真和补丁覆盖

Icarus 40 MHz / 150 MHz 功能测试通过：GBA/GB 连续读取、GB 奇数尾包、
原始写入、GBA 1024 字节页和未对齐首地址、GB 256 字节页、
双缓冲接收/编程重叠、描述符/载荷 CRC、超长/短包、地址溢出、
NOR 超时、读包截断、完整 RD/WR 脉冲和安全退出。
零延迟与 14 ns 回程预算都通过。
模式键任意一位损坏均被拒绝；原 Bacon 电源保持及两种 MCU 参数下的
功能兼容测试通过。主机描述符字节序和 IEEE CRC32 检查通过。

当前连接的是 GBA 卡；GB/GBC 部分尚无本轮换卡实测，不能把仿真当作实机结论。

补丁预期镜像均由现有导出流程生成：
- 地球冒险3，WAITCNT：14 处修改，33554432 字节，
  `/sdcard/地球冒险3.patched-2.gba`。
- 黄金太阳1，SRAM＋无电池＋WAITCNT：FLASH_V123，15 处 WAITCNT 修改，
  9699328 字节，`/sdcard/黄金太阳1 开启的封印.patched-3.gba`。
- 地球冒险3 的同一组合因 32 MiB 内空间不足被正常拒绝，未写卡。

## 记录与历史

原始测量：`docs/cpld_benchmark_0910.jsonl` 及同名 serial.log；
板上诊断：`docs/cpld_bringup_0910.serial.log`；
补丁导出：`docs/cpld_patch_0910.serial.log`。

容量和时序迭代保存在各 `docs/cpld_route_*/` 目录及本地 Git：
去除默认 MCU 数据平面、页掩码计算、分级校验/提交、独立 CRC、
RAM 数据选择、控制预计算和逐字节模式键匹配。
关闭 Quartus 针对代理 Cyclone 布局的速度复制，保留真实 AG32 的完整 Supra 流程。
被取消的拥塞布局和较大时序违例候选均未打包用于写卡。
初始下降沿硬件候选及对照记录保存在 `docs/cpld_hwtest_20260910/`。

用户在 `docs/AG32_实机测试对比_20260910.md` 中的批注未修改、未暂存。
